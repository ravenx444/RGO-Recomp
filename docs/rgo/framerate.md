# Unlocked frame rate (roadmap Tier 1, item 2)

Goal: present frames at 60 fps, the display's refresh rate or as fast as
possible, while the game's logic keeps its original rate, so gameplay speed
does not change. Like other N64Recomp ports, this uses RT64's frame
interpolation. RT64 renders extra frames between two game frames by blending
the camera and object matrices of the previous and the current game frame.

This page records what the investigation found, the plan, and what is done.
The cloud cannot run the game, so every gameplay-visible step still needs a
test on the owner's PC.

## Findings

### How the game makes a frame (decomp, ravenx444/Rocket-Group-Orion)

- **One logic step per rendered frame.**
  - `viFrameSync` (0x80054FBC, `gameVi.c`) waits for at least
    `gVideoDeltaTime` VIs: 2 normally (30 fps) and 3 after slow frames
    (20 fps). It then swaps buffers and returns the elapsed VI count.
  - Every tick function scales by that count: `camTick`, `trackDraw`,
    `frontUpdate`, object and overlay controls.
  - There are no catch-up loops, so one presented game frame equals one logic
    step of 2 or 3 VIs.
  - Interpolation must therefore use the real per-frame rate (60 / VIs), not a
    fixed 30.
- **F3DDKR matrices are complete MVPs.** The CPU multiplies everything by the
  camera. The microcode has matrix slots (`include/f3ddkr.h`):
  - **Slot 0** is the camera's view × projection, or the ortho matrix for the
    HUD. It is loaded by `camSetProjMtx` (0x80040AA4), `camStandardOrtho`
    (0x80040D64) and `camStandardPersp` (0x80040E64). Level geometry,
    world-space particles and shadows are drawn with it.
  - **Slot 1** is model × view × projection. It is loaded by
    `camPushModelMtx` (0x80041CA0), `camPushFloatModelMtx` (0x800419AC),
    `camScaleModelMtx` and `camPushMuzzleMtx`.
  - **Slot 2** is a billboard (rotation/scale) matrix anchored at a vertex
    that slot 0 transformed. It is used by `camDoSprite` and friends.
  - A "multiply" load (bit 0x80 of the parameter) multiplies a matrix by
    slot 0 on the RSP. No decompiled C code emits it. Presumably skinned
    models' bone matrices use it (`objPrintModelObject` and `makeModelGfx`
    are still ASM).
- **Split screen** draws every camera's scene in turn (`trackDraw` loops over
  `mainGetNumberOfCameras()`), so a frame can hold several slot-0 cameras.
- **Extra RSP tasks.** Besides the main frame, the game submits:
  - small off-screen tasks: 64×64 I8 shadow masks and other effect targets;
  - occasional effect tasks: clone (mode 3), refraction (mode 4) and motion
    blur (mode 5, overlay 48).

  Some effects read earlier framebuffers back. Scope lines, small digits and
  rain are also drawn **on the CPU into the framebuffer** after the RSP
  finishes.
- **Camera cuts** happen on cutscenes (`objCutCameraActive` 0x80011C2C),
  level changes (`mainChangeLevel` 0x8004665C), restarts and view changes.

### How RT64 interpolates (pinned 5473732)

- **Target rate.** `UserConfiguration::refreshRate` sets it:
  - Original: off.
  - Display: the swap chain's refresh rate.
  - Manual: `refreshRateTarget`, capped at the display rate.
- **Original rate.** RT64 infers the game's rate from how often the VI origin
  changes between `updateScreen` calls. This port calls `updateScreen` once
  per finished game frame, so RT64 would always read 60 and never
  interpolate. `State::setRefreshRate(rate)` before each task overrides that.
- **Frame generation.** For each game frame RT64 renders
  `target / original` frames, blending previous and current matrices with
  weights from its tick counter. The present thread spaces them at
  `1 / target` (`rt64_workload_queue.cpp`, `rt64_present_queue.cpp`).
- **Matching.** Every matrix is recorded with the "transform group" on top of
  RT64's ID stack:
  - an explicit ID with `G_EX_ORDER_LINEAR` pairs the Nth matrix of that ID
    with the Nth one in the previous frame;
  - `G_EX_ID_AUTO` uses heuristics;
  - `G_EX_ID_IGNORE` never interpolates.
  - Cameras (the projection group) with an explicit ID are matched only if
    the previous frame used the same ID. Their view is blended as a rigid
    body and their projection element by element.
  - Rectangles are never interpolated.
- **The camera must be separate.** RT64 blends world (model) matrices and the
  camera independently. A full MVP blended as a "model" matrix loses its
  perspective part. RT64 already supports games that insert an MVP
  (`RSP::modelViewProjInserted`): it stores the world matrix as
  `MVP × inverse(camera)`, while the CPU-side clip positions still use the
  MVP itself.
- **One task is one game frame.** RT64 matches each task (each
  `G_RDPFULLSYNC` workload) against the one before it. So JFG's off-screen
  tasks would sit between two main frames and nothing would match. When
  such a task is the last one before a present, the present thread also
  turns interpolation off for the frame.

### What the port did before this work

The F3DDKR shell (`src/renderer/rt64_f3ddkr.cpp`) gave RT64 every matrix as
a model matrix with an identity camera, tagged `G_EX_ID_IGNORE` with every
component SKIP. Interpolation was therefore impossible even with a higher
target rate.

### How other ports do it

Zelda 64, Banjo, Starfox, Goemon and Dinosaur Planet (N64Recomp):
- **Every frame:** they send `gEXEnable` and `gEXSetRefreshRate(60 / divisor)`.
- **Camera:** an explicit camera ID. All components are skipped for one frame
  on a cut, detected from a large jump against the predicted motion.
- **Objects:** IDs from the actor's spawn slot plus the limb index (Zelda:
  `0x1000000 + slot * 512 + limb`), linear order, decomposed interpolation.
  Position and rotation are skipped for a frame on teleports and spawns.
- **Terrain:** tagged IGNORE; the camera's interpolation moves it.
- **Particles:** one ID per pool slot. Effects that respawn every frame snap.
- **HUD:** aligned only (`gEXSetRectAlign`/`gEXSetViewportAlign`). Animated 2D
  is redrawn as ortho triangles inside tagged groups.

## Plan

Each phase is a separate, testable step. **Frame rate: Original leaves the
renderer exactly as before every phase.** The new code paths only run when a
higher frame rate is chosen.

1. **Phase 0: setting, rates and frame grouping.**
   - **Setting.** Video settings → Frame rate: Original / 60 / Display refresh
     / Unlocked, stored as `framerate=` in `video.ini`. The shell maps it to
     RT64's refresh rate:
     - 60 → Manual 60;
     - Display refresh → Display;
     - Unlocked → Manual with an uncapped target (below).
   - **Original rate.** The game reports its own rate for every task: 60 / the
     VIs since the previous game frame, so 30 or 20.
   - **RT64 patch (`cmake/rt64_frame_interpolation.cmake`), only while a target
     rate is set:**
     - Workloads that draw no screen-sized, presentable color image are
       *auxiliary*: shadow masks and effect targets. They render normally but
       do not advance RT64's game frame. They are not matched and do not
       reset its tick counter, frame counters or present candidates. Main
       frames are therefore matched with the previous main frame.
     - Unlocked (`refreshRateTarget` 1000) is not capped at the display rate.
       RT64's own budget check drops frames it can't render in time.
   - **FPS counter.** It now shows the game's rate and the displayed rate,
     counted at RT64's present hook.
2. **Phase 1: camera and world.**
   - **Camera split.** While interpolating, the shell gives slot 0 to RT64 as
     the camera (projection stack; RT64 splits view and projection). Every
     other matrix goes in as an inserted MVP, so RT64 derives its world
     matrix.
   - **Camera tags.** Each slot-0 load gets a camera ID from:
     - perspective or ortho;
     - screen-sized or off-screen target;
     - its order within the frame.

     Cameras blend in simple mode. A camera cut is detected in the shell (a
     position jump far beyond the previous step, a turn of more than 45°, or
     a field-of-view change) and skips that frame's blend.
   - **Result.** Level geometry, the sky and everything that stands still in
     the world move smoothly with the camera. Moving objects still step at
     the game's rate, because their matrices stay IGNORE.
3. **Phase 2: objects (experimental).**
   - **IDs.** Hooks on `camPushModelMtx` and `camPushFloatModelMtx` record the
     matrix address the game is about to write, together with an ID: the
     object transform pointer plus the push count for that transform this
     frame. The shell looks the ID up when it decodes that matrix.
   - **Blending.** Tagged matrices blend in decomposed mode with linear order.
     Position uses RT64's AUTO rule, which snaps on velocity jumps such as
     teleports, respawns and reused object slots.
   - **Bone matrices.** "Multiply" loads are already world-space. They take
     the ID of the last tagged object matrix plus their order within it, so
     a character's limbs move with its root.
   - **Opt-in.** `interpolate_objects=on` in `video.ini` (not on the page yet)
     until it has been seen in game.
4. **Phase 3: particles, billboards, shadows and effects.**
   - Sprites anchored through slot 2 and world-space particles (`partDraw`,
     IDs from `partlists` slots) need their own tags.
   - Shadows and world-space particle quads use raw world vertices and snap
     to the logic rate. Fixing that needs vertex velocities (`G_EX_VERTEX`) or
     redrawing them under a tagged matrix.
   - Lens flares (`camlightDraw`) should be redrawn per presented frame, or
     follow the camera.
   - Motion blur, refraction and clone tasks read earlier framebuffers back.
     They should probably snap: interpolation off while they run.
5. **Phase 4: HUD and CPU-drawn pixels.**
   - HUD quads drawn with `frontDrawObj` under the ortho camera keep their
     IGNORE tag (they don't move with the camera), so they are already
     stable.
   - CPU-drawn pixels (scope lines, digits, rain) appear only in the real
     game frames, because RT64 re-renders interpolated frames from display
     lists. They need to be redrawn on the GPU (extended GBI rectangles), or
     the frames that contain them must not be interpolated.

## Status

- [x] **Phase 0.**
  - The setting: `framerate=` in `video.ini`, plus the Frame rate row.
  - The game's rate per frame (`Rt64Shell::set_game_rate`).
  - The RT64 frame-grouping and Unlocked patch (`cmake/rt64_frame_interpolation.cmake`).
  - Displayed FPS counted at RT64's present hook (`Rt64Shell::display_presents`).
- [x] **Phase 1.**
  - The camera split (`apply_camera` and `select_matrix` in `src/renderer/rt64_f3ddkr.cpp`).
  - Camera IDs and cut detection (`include/jfg/renderer/frame_interpolation.hpp`).
- [x] **Phase 2 (experimental, off unless `interpolate_objects=on`).**
  - Hooks on `camPushModelMtx`, `camPushFloatModelMtx` and `camPushMuzzleMtx` (`interpolation_tag_dispatch` in `src/boot/native_boot.cpp`).
  - Tags checked against the matrix words.
  - Bone IDs for multiply loads.
- [ ] **Phase 3:** particles, billboards, shadows, lens flares, effect tasks.
- [ ] **Phase 4:** CPU-drawn pixels.

### Implementation notes

**Original changes nothing.** With Frame rate at Original:
- the shell runs exactly the old matrix path;
- RT64's target rate is 0, so the frame-grouping patch is inert;
- the object hooks are not installed.

**The camera split.**
- A slot-0 load becomes RT64's projection (`viewProjMatrixStack`), split into
  view and projection by RT64's `matrixDecomposeViewProj` when it is a
  perspective camera.
- Slot-0 draws use an identity model matrix.
- Every other matrix is an *inserted* MVP (`RSP::modelViewProjInserted`):
  - RT64 stores `MVP × inverse(camera)` as the world matrix for blending;
  - the CPU-side clip positions the shell uses for culling and billboard
    anchors still come from the MVP itself, so culling is unchanged.

**Camera IDs.**
- An ID is made from:
  - perspective or ortho;
  - screen-sized or off-screen color image;
  - its order among the frame's cameras of that kind.
- Cameras blend in RT64's simple mode.
- Every component is set to SKIP for one frame when `camera_cut` sees any of:
  - a jump over 400 units and four times the previous step;
  - a turn of more than 45°;
  - a field-of-view change of more than 25 %.

**Object IDs (phase 2).**
- The hook runs the game's own function, then records three things:
  - the matrix's physical address;
  - an FNV hash of its 16 words;
  - `object_group_id(object pointer, pushes since the last camera)`.
- The shell uses a tag only when the matrix it decodes hashes the same.
- Tagged matrices blend decomposed. Position is AUTO (snaps on velocity
  jumps); rotation and scale are blended.
- Multiply (bone) loads take `bone_group_id(last object, index)`.

**FPS counter.** The game sends both rates. The frontend shows
`60.0 FPS (game 30.0)` while the display shows more frames than the game
makes.

## Risks and what to look for

- **Off-screen task classification.** Tasks are classified by their color
  image (screen-sized and presentable or not). If an effect draws into a
  screen-sized buffer, the frame grouping may hiccup while that effect runs:
  one unblended frame.
- **Precision.** The camera split computes `MVP × inverse(camera)` in floats.
  On real frames it multiplies back to the same MVP. Z-fighting between
  decals and walls would show up as flicker only when Frame rate is above
  Original.
- **Lighting.** RT64 now sees real world matrices instead of MVPs. If any
  material uses RT64 lighting, it can look different above Original.
- **Pacing.** RT64 spaces the extra frames with a timer. The game's own
  pacing stays at 60 VIs per second, so a slow PC may show uneven frames
  rather than a slower game.
