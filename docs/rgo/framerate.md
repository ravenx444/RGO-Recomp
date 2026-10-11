# Frame rate (roadmap Tier 1, item 2)

Goal: the game itself runs at 60 frames per second, so it plays and feels
like 60, with gameplay at its original speed. Display rates above 60 come
from frame interpolation on top of that.

**Status.** Frame rate **True 60** runs the game's logic 60 times per
emulated second in menus, every opening cutscene and gameplay, at the
original game speed, measured headlessly (see [Measurements](#measurements)).
**Original** with *Original N64 slowdown* off holds a steady 30.
A PC that cannot keep up keeps the game's speed and gets fewer frames
instead of slow motion. The runtime now needs about half the host time it
did ([Host speed](#host-speed)). On this slow cloud VM, in real time,
True 60 runs at 57-60 in menus, light cutscene shots and the start of the
first level, 48-59 in the rest of the gameplay measured (it varies from run
to run), and falls back to 30 in the heaviest cutscene shots. Owner test
steps are near the end.

After the input stack was merged, the owner saw True 60 engage on some runs
and not others, things moving too fast at 60, the 20s in a firefight and
audio crackle; see
[After the input merge](#after-the-input-merge-owner-report-and-fixes).

## Why the first attempt did not work

PR #8 (`rgo/true-60fps`) forced the game's one-VI mode and overclocked the
emulated CPU 8x. The headless Linux build (below) measured it at 30 game
frames per emulated second, crashing in the opening cutscene:

| Branch / setting | Game frames per emulated second | Emulated time per wall second (this cloud VM) | Result |
|---|---|---|---|
| main, Original (console speed) | 30, 18-26 in heavy cutscenes | 0.54-0.83 | the console's own slowdown |
| PR #8, Original + slowdown Off (4x) | 30 | - | fail-closed at VI 424 (`unqualified-event-tie`) |
| PR #8, True 60 + slowdown Off (8x) | 30.0 (every frame 2 VIs) | 0.43-0.67 | fail-closed at VI 7983 in the opening cutscene (`unqualified-event-tie`) |

Three separate causes:

1. **The scheduler holds every frame to two VIs.** `runInOneFrame` makes
   `viFrameSync` wait for one VI, but the game's scheduler
   (`__scTaskComplete`, US 0x80050A08, decomp `sched.c`) keeps the frame's
   "graphics done" message until two VIs have passed since the previous one
   (`if (sc->frameCount <= 1) defer`). The main loop waits for that message,
   so frames still took two VIs. The decomp even notes it: "If you want to
   make the game 60FPS, change this to 2."
2. **The overclock made the host do more work, not less.** Production
   builds charge each guest instruction its VR4300 cycles
   (`jfg_original_timing_step`). Cutscenes busy-wait for the music
   (`amWaitForMidiSync` polls `osRecvMesg(NOBLOCK)` in a loop), and every
   poll is charged. At 8x, waiting one VI takes 8x as many polls, so the
   host executed 139 million guest instructions per emulated second instead
   of 52 million. Emulated time fell behind the wall clock: 30 game frames
   per emulated second became 15-20 per real second, in slow motion. That
   is the "N64 speed, 15-20 fps feel".
3. **The crash.** Device events (VI, AI, PI, SI, timer, RSP, RDP) are
   ordered by their Count deadlines, and two equal deadlines fail closed
   unless they are the RSP/RDP pair. With Count advancing by a fraction of a
   tick per instruction, two devices started a few instructions apart share
   a deadline. That trap is the cutscene crash.

The FPS counter showed presented frames (up to ~700 with Unlocked
interpolation), not game logic, so it hid all of this.

## Design

### The fast clock

`fast_clock` in `jfg/boot/original_timing_fast.h` and the runtime
(`src/boot/native_boot.cpp`). Used by True 60 / Display / Unlocked, and by
Original with *Original N64 slowdown* off. Slowdown On keeps the original
cost model unchanged (the faithful console timing, with its 20 fps drops).

- **CPU work costs almost no emulated time.** Every instruction advances
  Count by a flat 1/16 tick and skips the cache model, so even the heaviest
  frame finishes in a fraction of a VI. Polling loops on device registers
  still see time pass.
- **Waits skip to the next device event.** The idle loop already did. A
  thread that polls the same empty queue from the same call site 16 times in
  quick succession (`osRecvMesg` NOBLOCK, about 4,000 instructions apart at
  most) is waiting for an interrupt: Count jumps to the earliest device
  deadline. Polls a frame apart (`mainResetPressed`, `viFrameSync`'s check
  for pending VIs) never skip; skipping there stretched frames and put a
  cutscene out of step with its music until it waited for a sync that had
  already passed (a hang at VI 7095 found headlessly). Polling the VI line
  register (`__osViInit` waits for the top of the screen at boot) skips to
  the next VI the same way.
- **Skip runs.** Every guest instruction calls a timing hook. Under the fast
  clock an ordinary instruction only adds 1/16 of a tick, so the native
  check grants a run of instructions that cannot reach the next device
  deadline, and the generated hook just counts the run down
  (`jfg_fast_budget`, `jfg_fast_run_sync` in `original_timing_fast.h`). A run
  ends a few ticks before the deadline, at an MMIO access, at any COP0
  operation and when the dispatcher moves Count. It is only granted with no
  interrupt pending (masked ones included). FPU instructions only skip
  while the FPU is usable, so the coprocessor-unusable exception libultra
  uses to save FPU state still happens. Idle-loop boundaries, thread parks
  and CACHE instructions always reach the check.
  `JFG_RGO_NO_FAST_RUNS=1` checks every instruction again.
- **Equal deadlines are ordered** (`select_reference_event(..., order_ties)`)
  instead of failing closed.
- **Real-time floor.** CPU work is free in emulated time, so a slow host
  would otherwise play in slow motion. Count is kept at most a quarter VI
  behind the wall clock (anchored at each paced VI), so the next VI arrives
  while a slow frame is still running; the game counts two VIs and
  simulates two ticks, like the console and other N64Recomp ports. A stall
  longer than three VIs (shader compile, window drag) is skipped, not
  replayed; a burst of VIs made the game's scheduler declare its graphics
  task crashed. `JFG_RGO_DETERMINISTIC_CLOCK=1` turns the floor off for
  measurements.
- **No 20 fps hysteresis.** After any frame that took three or more VIs,
  `viFrameSync` switches the game to its 20 fps mode and only returns to 30
  after 31 faster frames (`gVideoDeltaTime`, `gameVi.c`). On the console
  that is the slowdown; with the real-time floor a single host hitch (a
  load, a shader compile) cost at least 1.5 s at 20 fps. Under the fast
  clock each `viFrameSync` runs with the game's own `gSkipVideoDeltaAdjust`
  (US 0x800FECAD, set by the game around loads) set, so a slow frame gets
  its extra VI and ticks once and the next frame is back at the normal rate.

### True 60

`true60_dispatch` in `src/boot/native_boot.cpp`, constants in
`jfg/renderer/frame_interpolation.hpp`.

- `viFrameSync` runs with `runInOneFrame` set (one VI per frame, update rate
  1), as before.
- `__scTaskComplete` runs with the scheduler's `frameCount` (OSSched +
  0x300) raised to 2, so the frame-done message goes out at once.
- The places that assume two ticks per frame stay fixed: texture
  scrollers run per two ticks, literal two-tick particle bursts use the real
  rate, and a frame without a new controller read reports no new presses.
- **The bug enemies' AI runs at 30** (`squads_function`). The squads AI
  (overlay 3: `SquaddieControl` 0x00310600, `SquadronControl` 0x00314D4C,
  `AIPointControl` 0x0030A110, `AIJunctionControl` 0x00316CB4,
  `squadsModuleTick` 0x00316990) counts per call, not per tick: a bug moves
  by its velocity, accelerates, gets drag and gravity, spins when knocked
  back and counts its fire delay and other timers by one per call; the
  squadron counts its reinforcement timer per call; the module ages the
  noises the bugs remember per call and flips an every-other-update flag.
  At one VI per frame bugs ran, strafed, tumbled, fired and spawned twice as
  fast while their legs (the animation does use the rate) moved at normal
  speed. True 60 now runs these five functions together once per two ticks
  with rate 2, exactly their 30 fps inputs, so bugs behave as on the console
  and their positions update 30 times a second (RT64 blends them above 60).
  Only calls that pass the frame's rate are batched; the level setup
  (`squadsInitialiseAfterObjects`) runs each bug once, untouched. The same
  half-rate table and odd-tick compensations fix the other per-call code
  the [True 60 audit](#true-60-audit) found (splashes, wakes, the aim
  stance, shurikens, lens flares, the hover tank, the slug boss's bolts).
  See [the owner's report](#after-the-input-merge-owner-report-and-fixes).
- **Governor** (`True60Governor`). As soon as 7 of the last 60 frames needed
  more than one VI (the host is too slow for 60 here), the game gets its own
  steady 30. Every frame also reports how long the host was busy with it
  (wall time minus the VI wait); a frame at 30 runs the same game step and
  draws the same picture, so that is about what a 60 frame costs. After a
  minimum time at 30 (2 s, doubling after each try that fails within 10 s,
  up to 30 s, back to 2 s after 10 s at 60) it checks the busy time 90 % of
  the last 60 frames stayed within: up to 15 ms it resumes 60, up to 20 ms
  it tries 60 at twice the minimum time, above that it stays at a steady 30
  (a 60 frame misses its VI after about 21 ms). A new level or cutscene shot
  tries 60 at once. A steady 30 looks better than frames of one, two and
  three VIs in turn.

### Settings apply live

The game rereads `video.ini` every 250 ms and applies it on the next frame:
the clock, True 60, interpolation and the counter. Going back to Original
also hands the game back its 30 fps mode (`gVideoDeltaTime` would otherwise
stay at 60 until the next level load). Only Anti-aliasing needs a restart,
and the settings page says so.

### FPS counter

`Game 60 | Render 60 | Display 144 | 6.9-7.0 ms`
(`jfg/renderer/frame_stats.hpp`):

- **Game**: game logic updates per second (`viFrameSync` calls). This is the
  rate the game simulates at, the only one that changes how it plays.
- **Render**: real game frames handed to the renderer per second.
- **Display**: frames the swap chain presented per second, blended frames
  included.
- **min-max ms**: the shortest and longest gap between two presented frames
  in the last half second. A wide spread is judder.

#### Diagnostics: FPS counter "Detailed"

The owner's first True 60 test showed `60.0 FPS`, but the game "felt like
interpolated 30". The plain counter can't tell why, so Video settings →
FPS counter → **Detailed** adds seven lines under the counter (all per
second, except the frame times and the last two lines):

```
Game 60 | Render 60 | Display 60 | 16.5-16.9 ms
logic 60.0 | ticks 60.0
drawn 60.0 | new 60.0 | camera 60.0 | repeats 0.0
input 60.0
game frame 16.6-16.8 ms
display frame 16.5-16.9 ms
host 9.8 ms, longest 14.2 ms | gfx 6.0 tasks, 3.1 ms | VI wake +0.1 ms
True 60: 60, late 0/60, busy 11.0 ms
```

- **logic**: `viFrameSync` calls, one per game frame. **ticks**: the VIs
  those frames handed to the game logic (`delayDat`). Ticks are 60 at full
  speed in every mode. Logic is 30 at Original and 60 at True 60; a 30 here
  at True 60 means the one-VI mode isn't engaged.
- **drawn**: game presents that show a newly drawn picture. **repeats**:
  presents that show a picture already shown.
- **new**: drawn frames whose matrices differ from the previous drawn
  frame's. The renderer shell hashes every matrix a frame loads
  (`Rt64F3ddkrStats::scene_hash`). **camera**: the same for the camera
  (slot 0) matrices alone. Both only mean something while the camera or
  objects are moving: walk and turn while reading them.
  - At True 60, while turning, `new` and `camera` should be about 60. A
    value near 30 means the game logic updates the scene (or the camera)
    only every other frame, even though it draws 60 pictures.
- **input**: controller samples per second. `joyRead` only starts the next
  controller read after the previous one completed, so this can be lower
  than the logic rate. Mouse look (PC controls) is applied once per sample:
  input at 30 while logic runs at 60 makes mouse turning step at 30.
- **game frame**: the shortest and longest gap between game presents in the
  last half second. **display frame**: the same between swap-chain
  presents (RT64's present hook). Steady 60 is about 16.7 ms for both; gaps
  alternating near 0 and 33 ms are frames presented in pairs, which looks
  like 30.
- **host**: how long the PC was busy with each game frame in the last half
  second, average and longest: the wall time between two game frames minus
  the time the emulation thread slept waiting for the next VI. At True 60 a
  frame must fit in one VI (16.7 ms, a few ms more with the real-time
  floor's slack); at 30 in two. **gfx**: graphics tasks per game frame (the
  main frame plus off-screen tasks such as one shadow mask per character)
  and the time their submission to the renderer took per game frame, the
  renderer's GPU syncs for shadow masks included. If host is high and gfx
  ms is a large part of it, the renderer is the bottleneck; if gfx ms is
  small, the game's code and the runtime are.
- **VI wake**: how late the emulation thread woke from its wait for a VI,
  at worst in the last half second. A few tenths of a millisecond is
  normal; several milliseconds mean the operating system wakes it late,
  which makes frames miss their VI however fast the PC is.
- **True 60**: `60` with the late frames of the last 60 (7 or more make it
  fall back), or `holding 30`, or `off` (Frame rate Original). **busy** is
  the busy time 90 % of the last 60 frames stayed within, which decides when
  60 resumes (up to 15 ms) or is tried again (up to 20 ms).

The numbers come from `include/jfg/renderer/frame_stats.hpp` (tested in
`jfg.frame_interpolation`). The game posts them as `WM_APP + 25` to
`WM_APP + 30`; `fps_details=on` in `video.ini` shows them. **logic** is the
same count as **Game**, and **display frame** the same range as the plain
counter's.

### Frame pacing

- VIs are paced to the audio-anchored wall clock with a precise wait (sleep
  until 2 ms before, then yield), instead of a plain sleep that can wake a
  millisecond or more late (`jfg/runtime/precise_wait.hpp`).
- Above 60, RT64 blends frames between game frames. Blending only the camera
  made objects the camera follows (the player) step at 60 against a smoothly
  moving scene, which reads as judder. **Blend moving objects** (on by
  default) blends object matrices too; turn it off if an object glitches.

### Host speed

True 60 runs twice as many game frames per second, so the host has to run
the game's code twice as fast as at 30. Profiling the headless build
(`perf`, True 60, deterministic clock) found where the time went:

| Where | Share of host time before | Change |
|---|---|---|
| Single-step trap after every device register access | 35-42 % | **One-trap MMIO** (below) |
| RDRAM snapshot per graphics task (`copy_changed_snapshot_bytes`) | 10-24 % | unchanged, see Risks |
| Handing the baton between guest threads (futex wake-ups) | 13-20 % | **brief spin before sleeping** |
| `dispatch` and the generated code | the rest | unchanged |

- **One-trap MMIO** (`jfg/boot/mmio_instruction.hpp`, `mmio_emulate_move` in
  `native_boot.cpp`). Device registers sit on guarded pages. A guest access
  faulted, the handler presented the register, then single-stepped the host
  instruction and took a second trap to apply a write. The game makes about
  11,000 register accesses a second at True 60 (`osSetIntMask`, the
  exception handler, thread switches, DMA), and the single-step trap is
  expensive. Now the handler decodes the faulting instruction; when it is a
  plain 32-bit `mov`, `movsxd` or `mov dword, imm` (all of them in the GCC
  build), it does the load or store on the register page itself, finishes
  the access and skips the instruction. The register page and the order of
  start, access and finish are unchanged, so the game sees exactly the same
  values: a deterministic run gives a frame trace identical to the
  single-step one, Count and instruction counts included. Any other
  encoding still single-steps. `JFG_RGO_MMIO_SINGLE_STEP=1` turns it off.
- **Baton spin** (`src/boot/executor.cpp`). Each guest thread is a host
  thread and exactly one runs at a time. A thread waiting for the baton now
  spins for up to 50 microseconds before it sleeps, so quick hand-offs skip
  the futex wake-up. The order of execution is unchanged.
- **The governor starts over in a new scene.** A heavy cutscene that cannot
  hold 60 grows the governor's pause up to 30 s; the gameplay after it used
  to stay at 30 for that long. The pause now resets when the level changes.

Unpaced throughput (emulated seconds per wall second, `--unpaced`; above 1.0
is headroom for real time) on this cloud VM, True 60:

| Segment (first VI) | One-trap MMIO | Single-step (before) |
|---|---|---|
| Title and file select (VI 43) | 2.18 | 1.15 |
| Name keyboard (VI 1085) | 2.95 | 1.28 |
| Opening cutscene, level 322 (VI 1288) | 1.64 | 0.74 |
| level 324 (VI 2261) | 1.19 | 0.57 |
| level 323 (VI 2937) | 1.07 | 0.62 |
| level 325 (VI 5281) | 0.59 | 0.34 |
| level 323 (VI 6213) | 0.96 | 0.52 |
| level 326 (VI 6539) | 1.63 | 0.81 |
| level 323 (VI 7179) | 0.71 | 0.39 |
| level 327 (VI 8697) | 1.19 | 0.66 |
| level 323 (VI 9819) | 1.13 | 0.54 |
| Level intro, level 127 (VI 10510) | 1.83 | 0.92 |
| Gameplay, level 365 (VI 10653) | 1.57 | 0.82 |
| Gameplay, level 93 (VI 10983) | 1.62 | 0.81 |
| Gameplay, level 92 (VI 12622) | 1.33 | 0.65 |
| Whole run | 1.16 | 0.60 |

Both runs give the same frame trace (VIs, ticks, Count, instructions) as
the paced deterministic run. Gameplay frames take 10-12 ms median here
(levels 365, 93, 92); about 1 % take longer than the ~21 ms after which a
frame misses its VI in real time. Levels 325 and the second long level 323 shot are the heaviest
cutscene stretches (about 8 graphics tasks a frame).

## After the input merge: owner report and fixes

`main` at c96b25b merged the 60 fps work (#13) and the input stack (#9-#12)
textually, without ever running them together. On the owner's PC: (1) one
run stayed at 30 in every setting and level, the next ran 60, with no
change; (2) at 60 some things moved faster than normal; (3) a firefight with
about four bugs dropped into the 20s; (4) audio crackled now and then, and
once that came before a crash. Branch `rgo/framerate-fixes`.

**The merge itself.** The Windows build compiles, but the input code broke
the headless Linux build (the gamepad functions were outside the `_WIN32`
guard of the state they use), so the merged tree had never been measured.
The headless build now compiles and also runs the SDL gamepad thread, so
its measurements include the input stack. Nothing in the input code touches
the True 60 decision (the setting, `video_managed`, the governor), the VI
clock or the scheduler.

**1. True 60 on some runs and not others.** The owner confirmed both runs
used the same build of `main`, nothing rebuilt, so it varies from run to
run in one binary. True 60 itself engages from the setting every frame;
what then decides between 60 and 30 is only the governor, and the old
governor decided from late frames alone: 7 late frames out of 60 meant 30
for 4 s, then a try, doubling to 30 s pauses after failed tries, reset only
by a new level or shot. So anything that makes frames late for a whole run
keeps it at 30 in every level. Two run-to-run causes fit:
- **Imprecise VI waits.** Each VI waits with a sleep until 2 ms before it,
  then spins. On Windows that sleep is only as precise as the process's
  timer resolution. SDL asks for 1 ms, but Windows 11 does not always grant
  it, for example to a process it considers occluded or inaudible, and
  without it a sleep can wake up to 15.6 ms late. Every frame then misses
  its VI no matter how fast the PC is, and whether it happens depends on
  the state of the windows and audio when the game starts. The wait now
  uses a high-resolution waitable timer (Windows 10 1803 and later), which
  wakes within about half a millisecond whatever the timer resolution, as
  the SDL input thread already did. The Detailed counter's new `VI wake`
  figure shows how late the waits woke (a few tenths of a ms normally).
- **A slower host for that run:** the first run after a build compiles
  RT64's shaders on the CPU, Windows can put the emulation thread on an
  efficiency core, background load.

Neither can be confirmed without numbers from the PC; the counter now shows
them. The input code is not involved in the decision: it does not touch
the setting, the governor, the VI clock or the scheduler, and its thread
costs no measurable time (item 5). Three changes:
- The governor resumes on evidence: each frame's host busy time (wall time
  minus the VI wait) shows whether 60 fits, so 60 comes back within 2 s
  once the host is fast enough, instead of after up to 30 s
  ([True 60](#true-60)).
- VI waits use a high-resolution waitable timer on Windows
  (`paced_sleep_until`).
- The Detailed counter now shows the host time per frame, the graphics
  tasks and their submission time per frame, how late the VI waits woke,
  and the governor's state, so a photo of it tells which case it is
  ([Detailed](#diagnostics-fps-counter-detailed)): `holding 30` with a low
  `host` and a `VI wake` of several ms is the timer; a `host` above about
  15 ms is the PC being slow there.

**2. Things faster than normal at 60: the bug enemies.** The engine's object
loop, physics, animation, particles, explosions, projectiles, weather and
HUD scale by the tick rate (checked in the decomp and the US asm). The bugs'
AI (overlay 3, the squads module) does not: movement, acceleration, drag,
gravity and knock-back spin, the fire delay, several AI timers, squadron
reinforcement timers and the memory of noises all count per call. At 60
bugs ran, strafed, tumbled, fired and got reinforcements twice as fast
while their legs moved at normal speed (they slid). True 60 now runs the
squads AI once per two ticks with rate 2 ([True 60](#true-60)). The
headless autopilot does not meet bugs, so this needs the owner's test.
The rest of the per-call code is covered by the
[True 60 audit](#true-60-audit) below.

### True 60 audit

Everything else that could run twice as fast at one tick per frame, checked
in the US asm (overlay calls resolved through the ROM's relocation tables):
scans of every overlay and main-code function for fields changed by a
constant per call, constant float steps, position += velocity, and control
functions that never read their rate, then the hits read by hand.

Rare's object code is rate-correct almost everywhere: integration runs in a
loop once per tick, steps are multiplied by the rate, drag is
`Powerf(decay, rate)`. Overlays never read `delayDat` themselves; they get
the rate as an argument.

Fixed (`frame_interpolation.hpp`, "True 60"):

| What | Function | Per call | At 60 before | Fix |
|---|---|---|---|---|
| Bug enemies | squads AI, overlay 3 | movement, timers, fire delay, spawns | twice as fast, sliding | half rate |
| Water splash | `splashControl` 0x80028034 | 3 droplets | burst in half the time | half rate |
| Wakes | `wakeUpdateRipple` 0x8006B090 (and `wakeUpdate`) | fade ±64, one trail segment | half as long, fade twice as fast | half rate |
| Aim stance and fire spin-up | `controlUpdatePlayerAim` 0x80039FCC | +0x1F4, +0x1F8, lock-on nibble -1 | stance drops after 0.25 s, not 0.5 s | counters raised by one on odd ticks |
| Shuriken | `ShirukenControl` 0x07A004EC | spin +0x1D6B, tumble after a bounce | spins and tumbles twice as fast | step taken back on odd ticks |
| Lens flares | `camlightVisibilityCheck` 0x80024044 | fade ±8, round-robin cursor | fade twice as fast | fades and cursor restored on odd ticks |
| Hover tank | `hoovertankControl` 0x06A00290 | spin speed ±1 | spins up and down in half the time | one step taken back on odd ticks |
| Slug boss bolt | `FlowerBoltControl` 0x09501450 | arming counter +1 | arms after 0.17 s, not 0.33 s | one step taken back on odd ticks |

"Half rate" runs the function every other frame with rate 2 (its 30 fps
inputs). "Odd ticks" are the frames those functions skip; functions that
must still run every frame (aim angles, shuriken motion, the lens flares'
z-buffer checks) run then with their per-call step undone, so the step
happens once per two ticks. Only calls that pass the frame's rate are
batched; a step is only taken back when the call made exactly that step.

Checked and already rate-correct: `sidekickControl` (its +0x1C/+0x1E timers
run in a loop once per tick), `rumbleTick` (only the motor's duty-cycle
toggle runs per call, which you can't feel), the day/night clock
(`dayUpdate`, overlay 23, gets exactly `delayDat` while `dayfactor` is 256
and 0 when time is frozen; nothing is lost at one tick), Mizar and its
attacks, Mantis, the boy/girl/dog boss fights, the bosses of overlays
138-155 (their own per-call counters only run from the squads AI, which now
runs at 30), FatBob, maggots, zombie flobs, spiders, cyborgs, missiles,
laser bolts, mines, grenades, the hovership, the players' controls and
`controlUpdateWeapon`, pickups, invincibility, chests, doors, bridges,
texture scrollers, particles, sparks, droplets, blood, rain, the screen
fade, the HUD message queue and the menus and pause screens.

Left as they are (minor or meaning not established): a player state counter
in overlays 15-17 (`+0x18A`, rises by one per call to 240, in what looks
like a death or gasp state), `twisterControl` (overlay 131, +0x11),
`refuelControl` (overlay 100, +0x10), `rockstormControl` (overlay 68,
+0x70), `mrhintsControl` (overlay 32, a two-call window), three 16-slot
cache rings at the end of `objObjectsTick` (entries expire after 0.27 s
instead of 0.53 s), the sprint and duck-shoot mini-games, and the insides
of a few very large functions (`func_8002CF78`, `menuCamControl`, the
reflection overlay) beyond what the scans cover.

**3. The 20s in a firefight.** A game frame at 30 takes three VIs (20 fps)
when the host needs more than about 37 ms for it; at that point no
scheduling helps, only host speed. Two things made it worse than it had to
be:
- The old governor kept trying 60 every 4-30 s even when the scene was far
  too heavy, and each try lasted a full 60 frames of one, two, three and
  more VIs (several seconds at 10-25 fps on a slow PC). It now falls back
  as soon as 7 of the last 60 frames are late and does not try 60 again
  while the busy time says a 60 frame cannot fit.
- The bugs fired and spawned twice as often (item 2), which adds work.

  What the host spends its time on in a heavy scene: every graphics task
  (one per character shadow mask plus the main frame) copies the changed
  parts of the 4 MB RDRAM image, comparing all of it; headlessly that is
  about a quarter of all CPU time in heavy cutscenes (`memcmp`/`memmove` in
  `perf`), and the Windows renderer compares and copies it once more and
  waits for the GPU for each shadow mask. The counter's `gfx` part shows
  that cost on the PC; reducing it is the next host-speed step (see
  [Risks](#risks-and-what-to-look-for)).

**4. Audio crackle.** The audio queue holds about 100 ms. When emulated time
falls more than that behind real time (a hitch: loading, a shader compile,
a very slow frame), the device runs dry, pauses and restarts after 100 ms
of new audio: a crackle. SDL's dummy driver plays 3-12 % too fast or slow,
so the headless build now has an exact wall-clock sink
(`JFG_RGO_WALL_AUDIO=1`, the default in `rgo_headless_run.py`). Measured
over the opening 250 s: on this VM 2-7 underruns at True 60; on a host
about half as fast 6-13 at True 60 and 18 at Original (30), so it is not
specific to True 60, it is host speed and hitches. An underrun is not a
crash: no audio path fails closed on one (the AI submission ring holds 2048
buffers). The crash after the crackle needs its `crash.log` to say what it
was.

**5. Does the input stack cost host time?** No measurable cost. The SDL
thread polls at about 500 Hz and used 2 % of one core on Linux without a
controller; reading a snapshot and the gyro on the emulation thread took at
most 1.6 microseconds per controller sample, and no lock is held across an
SDL call. The first controller read starts SDL's joystick subsystem and
waits for it (once, at boot; on Windows that can take a fraction of a
second). Windows-specific costs (XInput and HIDAPI polling) run on the SDL
thread, not the emulation thread.

## Headless measurement

The live runtime builds headless on Linux (CMake `JFG_HEADLESS_LIVE=ON`):
the same guest CPU, OS, device clocks and audio pacing as the Windows build,
without a window or GPU renderer (`src/renderer/rt64_shell_headless.cpp`).
Guest memory and MMIO trapping use `memfd`/`mmap` and SIGSEGV/SIGTRAP in
place of Windows file views and guard pages; `headless_win32_compat.hpp`
gives the few Win32 calls a Linux meaning.

1. Generate the code from the ROM (Linux): `python3 scripts/generate_from_rom.py
   --rom <rom> --workspace <ws> --dependency-root <deps>` (the
   `build_from_rom.py` dependencies at their `dependencies.lock.json`
   commits).
2. Configure and build:
   `cmake -S . -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
   -DJFG_WARNINGS_AS_ERRORS=OFF -DJFG_ENABLE_GENERATED_CODE=ON
   -DJFG_BUILD_PHASE6_NATIVE_BOOT=ON -DJFG_BUILD_PHASE8_LIVE=ON -DJFG_HEADLESS_LIVE=ON
   -DJFG_GENERATED_ROOT=<ws>/normalized -DJFG_PHASE6_LIBULTRA_IDENTIFICATION=<ws>/libultra.json
   -DJFG_PHASE8_PRIVATE_AUDIO_ADAPTER_ROOT=<ws>/audio
   -DJFG_PHASE8_PRIVATE_AUDIO_RUNTIME_INCLUDE=<deps>/N64ModernRuntime/librecomp/include`,
   then `ninja -C <build> jfg-native-boot` (needs SDL2 development files).
3. Run: `python3 scripts/rgo_headless_run.py --executable <build>/jfg-native-boot
   --rom <rom> --mode true60 --vis 15000 --deterministic --out <private dir>`.
   Modes: `original-slowdown`, `original`, `true60`, `display`, `unlocked`.
   `--switch 4000:original` rewrites `video.ini` mid-run. `--deterministic
   --unpaced` also drops the wait for the wall clock at each VI, so
   `emulated_per_wall` in the report is the host's speed (above 1.0:
   headroom for real time).

The run uses `JFG_RGO_AUTOPILOT=newgame` (`jfg/rgo/headless_autopilot.hpp`):
it reads the front-end state from RAM and presses A through the title and
the name keyboard, then walks forward and turns in gameplay. It reaches the
first level identically at every setting. `JFG_RGO_FRAME_TRACE` writes one
row per game frame (VI, ticks, Count, wall time, front-end mode, level);
`scripts/rgo_frame_rate.py` turns it into the numbers below.
`JFG_RGO_COUNTER_LOG=1` prints the FPS counter to stderr, with the host
line, the audio queue and underruns. The trace also has `busy_us` (host time
of the frame that just ended), `true60` (0 off, 1 at 60, 2 holding 30) and
`audio_underruns`. `JFG_RGO_WALL_AUDIO=1` (the script's default) plays into
an exact wall-clock sink instead of SDL's dummy driver. The headless build
runs the SDL gamepad thread like the Windows build. To simulate a slower
PC, pin the run to two CPUs shared with two busy loops
(`taskset -c 2,3 stress-ng --cpu 2` next to `taskset -c 2,3 python3
scripts/rgo_headless_run.py ...`): about half this VM's speed.

Keep the ROM, the generated code, the build and every output outside the
repository.

## Measurements

Headless build on the 4-vCPU cloud VM. Each run starts a new game: title,
name keyboard, every opening cutscene, then the first gameplay levels, for
15,000 VIs (250 s of emulated time). **Game frames per emulated second** is
`viFrameSync` calls per 60 VIs of emulated time, from the frame trace. The
runs use `--deterministic` (no real-time floor), so the host's speed does
not enter and the numbers are the game's own rate. The autopilot takes the
same path at every setting.

| Segment (front-end mode, level, first VI) | PR #8, True 60 | True 60 | Original, slowdown Off | Original, slowdown On |
|---|---|---|---|---|
| Title and file select (mode 2, VI 43) | 30.0 | 60.0 | 30.0 | 30.0 |
| Name keyboard (mode 24, VI 1085) | 30.0 | 60.0 | 30.0 | 30.0 |
| Opening cutscene, level 322 (VI 1288) | 30.0 | 60.0 | 30.0 | 30.0 |
| Opening cutscene, level 324 (VI 2261) | 30.0 | 60.0 | 30.0 | 25.7 |
| Opening cutscene, level 323 (VI 2937) | 30.0 | 60.0 | 30.0 | 25.2 |
| Opening cutscene, level 325 (VI 5281) | 30.0 | 60.0 | 30.0 | 18.0 |
| Opening cutscene, level 323 (VI 6213) | 30.0 | 60.0 | 30.0 | (short) |
| Opening cutscene, level 326 (VI 6539) | 30.0 | 60.0 | 30.0 | 30.0 |
| Opening cutscene, level 323 (VI 7179) | 30.0, **trap at VI 7983** | 60.0 | 30.0 | 18.8 |
| Opening cutscene, level 327 (VI 8697) | - | 60.0 | 30.0 | 28.9 |
| Opening cutscene, level 323 (VI 9819) | - | 60.0 | 30.0 | 26.6 |
| Level intro, level 127 (mode 5, VI 10510) | - | 60.0 | (short) | 30.1 |
| Gameplay, level 365 (mode 16, VI 10653) | - | 60.0 | 30.0 | 30.1 |
| Gameplay, level 93 (VI 10983) | - | 60.0 | 30.0 | 30.0 |
| Gameplay, level 92 (VI 12622) | - | 60.0 | 30.0 | 28.1 |

- **True 60:** 14,147 game frames in 14,985 VIs. 14,130 frames took one
  VI; the other 17 are the first frame after each level load (two VIs, one
  six). Game time (sum of ticks) over emulated time: 1.00 in every segment,
  so the game runs at its original speed. No trap; the run reaches the VI
  target. Display and Unlocked give the identical trace.
- **Original, slowdown Off:** 7,079 frames in 14,984 VIs, every one two VIs
  (except one load frame): a steady 30, including the cutscenes where the
  console drops to 18. No trap.
- **Original, slowdown On** keeps the console's timing: its trace matches
  the run before this change frame for frame (same VIs, same ticks), so the
  fast clock leaves it alone.
- The host speed work (one-trap MMIO, baton spin) and the merge with the
  Detailed counter changed nothing the game sees: True 60, Unlocked,
  Original and slowdown On give the same frame trace (VI, ticks, Count,
  guest instructions, mode, level) before and after, frame for frame.
- **PR #8** ran True 60 at 30.0 (every frame two VIs) and stopped at VI 7983
  in the opening cutscene (`unqualified-event-tie`); with Original and
  slowdown Off it stopped at VI 424.

**Live switching.** One run starts at True 60 and rewrites `video.ini`
during play: at VI 4000 to Original (slowdown Off), at VI 7000 back to
True 60, at VI 12000 to Original with slowdown On. Each change took effect
within 4-27 VIs (the script polls the trace every half second): the game ran
at 60, then 30 (levels 325 and 323), then 60 again from the next shot on,
then at the console's own rate in level 92 (28.1, frames of two and three
VIs). No trap.

**Real time.** Without `--deterministic` the real-time floor and the True 60
governor work as on a PC. This VM is slow: True 60's gameplay frames take 10-12 ms
median (see [Host speed](#host-speed)), a desktop CPU should do better.
Game frames per wall second, 15,000 VIs each:

| Segment (first VI) | True 60 | Original, slowdown Off |
|---|---|---|
| Title and file select | 59.3 | 29.8 |
| Name keyboard | 58.8 | 30.0 |
| Opening cutscene, level 322 | 58.8 | 29.9 |
| level 324 | 41.4 | 29.8 |
| level 323 | 35.2 | 29.7 |
| level 325 (heaviest) | 25.9 | 28.6 |
| level 323 | 35.6 | 29.9 |
| level 326 | 59.8 | 29.9 |
| level 323 (second heaviest) | 29.3 | 27.1 |
| level 327 | 42.2 | 29.7 |
| level 323 | 33.8 | 29.9 |
| Level intro, level 127 | 57.2 | 30.0 |
| Gameplay, level 365 | 57.8 | 30.0 |
| Gameplay, level 93 | 49.9 | 29.8 |
| Gameplay, level 92 | 47.8 | 29.8 |

- Game time over emulated time is 1.00 in every segment of both runs, and
  emulated time over wall time 0.99-1.00: the game keeps its speed. Where
  this VM is too slow, the frame rate drops, not the speed.
- True 60 below 60 means the governor ran those stretches at the game's own
  30 (frames of two VIs) after more than 6 of 60 frames were late; heavy
  cutscene shots on this VM. Two earlier runs of the same code measured
  54-59 in gameplay: this VM's speed varies from run to run.
- Original with slowdown Off holds 30 except in the two heaviest shots,
  where this VM occasionally needs three VIs for a frame. With the 20 fps
  hysteresis still active (before the `gSkipVideoDeltaAdjust` change) the
  same run measured 21-28 in cutscenes and 26 overall.

### After the input merge (rgo/framerate-fixes)

Real time, True 60, 15,000 VIs from the title to the first levels, game
frames per wall second. `main` is c96b25b with only the headless build fix;
the branch adds the governor and squads changes. On this VM gameplay holds
60 either way (no bugs on the autopilot's route); the cutscene shots vary
by several fps from run to run.

| Segment (first VI) | main | branch |
|---|---|---|
| Title and file select | 59.6 | 59.5 |
| Opening cutscene, level 322 | 59.6 | 58.7 |
| level 324 | 49.4 | 49.2 |
| level 323 | 47.7 | 46.8 |
| level 325 (heaviest) | 30.0 | 29.9 |
| level 326 | 59.7 | 59.2 |
| level 323 (second heaviest) | 39.0 | 31.2 |
| level 327 | 42.5 | 38.8 |
| level 323 | 48.0 | 49.7 |
| Gameplay, levels 365 / 93 / 92 | 59.5 / 59.4 / 59.4 | 59.7 / 59.1 / 59.6 |

The cutscene averages mix 60 and 30: `main`'s are a little higher because
the old governor kept trying 60 there, in bursts of uneven frames.

**A host half as fast** (the run pinned to two CPUs shared with two busy
loops), True 60, two runs each of the old and the new governor (both with
the new diagnostics and the exact audio sink):

| | old governor | new governor |
|---|---|---|
| Gameplay: game frames per wall second | 30.9-31.6 | 29.9-30.1 |
| Gameplay: switches between 1-VI and 2-VI frames | 443-456 | 193-248 |
| Gameplay: frames of 3+ VIs | 7.5-9.5 % | 6.1-6.7 % |
| Cutscenes: frames of 3+ VIs | 23.8-24.8 % | 26.5-28.1 % |
| Heaviest shots (325, 323): game frames per wall second | 14.4-20.0 | 14.1-17.5 |
| Heaviest shots: host busy per frame, median | 44-63 ms | 52-62 ms |
| Audio underruns in 250 s | 8-9 | 6-13 |

- In gameplay the new governor holds a steady 30 instead of mixing in 60
  tries: half the switching and fewer 3-VI frames. Original (30) on the
  same host: 29.2-29.9 in gameplay, 18 underruns.
- The heaviest shots need 50-60 ms of host time per frame here, so they run
  at 15-20 fps with either governor; only host speed changes that.
- Underruns vary from run to run as much as between the governors. Game
  time over emulated time stays 1.00 except in the heaviest shots (0.97-0.99
  on this host with either governor: frames that need far more than three
  VIs lose a little game time).
- No trap in any run; every run reached its VI target.

## Frame interpolation above 60

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

### Plan

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

### Status

- [x] **Phase 0.**
  - The setting: `framerate=` in `video.ini`, plus the Frame rate row.
  - The game's rate per frame (`Rt64Shell::set_game_rate`).
  - The RT64 frame-grouping and Unlocked patch (`cmake/rt64_frame_interpolation.cmake`).
  - Displayed FPS counted at RT64's present hook (`Rt64Shell::display_presents`).
- [x] **Phase 1.**
  - The camera split (`apply_camera` and `select_matrix` in `src/renderer/rt64_f3ddkr.cpp`).
  - Camera IDs and cut detection (`include/jfg/renderer/frame_interpolation.hpp`).
- [x] **Phase 2: objects (on by default since true 60; Blend moving objects).**
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

**FPS counter.** See [FPS counter](#fps-counter).

## History: overclock and forced one-VI mode (superseded)

PR #7 (`rgo/no-n64-slowdown`) overclocked the emulated CPU 4x; PR #8
(`rgo/true-60fps`) forced `runInOneFrame` with an 8x overclock. Both kept
the per-instruction cost model, which caused the problems in
[Why the first attempt did not work](#why-the-first-attempt-did-not-work).
The fast clock replaces the overclock; True 60 keeps the one-VI mode and the
fixes for code that assumes two ticks per frame, and adds the scheduler
release and the governor.

The hand-off notes left on `rgo/true-60fps` (commit 79cf1dd) listed the
owner's results and open questions. Measured headlessly since:
- *Host throughput* (the leading hypothesis there) was part of it: the
  per-instruction cost model made waits expensive, and device register
  traps took 35-42 % of host time ([Host speed](#host-speed)). The other
  part was the scheduler holding every frame to two VIs, which no host speed
  fixes ([cause 1](#why-the-first-attempt-did-not-work)).
- *The opening-cutscene crash* was the fail-closed `unqualified-event-tie`
  (equal device deadlines), reproduced at VI 7983 at True 60 and VI 424 at
  Original with slowdown Off, and gone with ordered ties.

## Owner test steps

Build from the PR branch (`rgo-setup.ps1`), start a **new game** and turn on
Video settings → FPS counter (On, or Detailed for the extra lines). The
counter reads `Game | Render | Display | shortest-longest frame gap`. The
opening cutscenes start right after the name entry and run about 2 min 40 s
before the first level; the old crash came about 1 min 50 s in.

1. **True 60, VSync On, 60 Hz display.**
   - Menus, name entry, first cutscene shots and the first level:
     `Game 60 | Render 60 | Display 60 | 16.x-16.x ms` (a range within about
     a millisecond of 16.7 means smooth). Detailed: `logic 60.0 | ticks
     60.0`, and while turning `new` and `camera` near 60.
   - The game must play at normal speed (walking, music and cutscene
     dialogue in step).
   - Heavy cutscene shots, if the PC cannot hold 60 there: `Game 30 | Render
     30 | Display 30 | 33.x ms`, Detailed `True 60: holding 30`. It goes
     back to 60 within about 2 s once Detailed `busy` is 15 ms or less. A
     new shot or level starts at 60. Detailed `ticks` stays at 60 either
     way.
   - The opening cutscene plays through to the first level without a crash.
   - Quit and start the game again five times, once with the window in the
     background while it starts: every run must reach 60 in the menus and
     the first level. If a run stays at 30, photograph the Detailed counter
     (the `host`, `VI wake` and `True 60` figures say why).
   - **Normal speed at 60:** compare True 60 against Original (switch live)
     for: bugs (running, strafing, shots per second, how far they fly when
     hit, reinforcements; legs match movement, no sliding); how long the
     aim stance holds after a target lock drops; wakes behind you when
     wading (length and fade); water splashes; lens flares fading in and
     out; thrown shurikens' spin; later, the hover tank spinning up and the
     slug boss's flower bolts. Each must look the same at both settings.
2. **Original, Original N64 slowdown Off:** `Game 30 | Render 30 | Display
   30 | 33.x ms` everywhere, including the cutscenes. A hitch (a load) may
   show one long frame; it must not stay at 20.
3. **Original, slowdown On** (the console's timing): `Game 30` in menus and
   the first level; heavy cutscene shots drop to `Game 18`-`26`, as on the
   console.
4. **Display, 144 Hz display:** `Game 60 | Render 60 | Display 144 |
   6.9-7.0 ms` (Display follows the refresh rate). Watch your character while
   turning: it should move as smoothly as the scenery (Blend moving objects On).
5. **Live switching:** change Frame rate and N64 slowdown while playing; the
   counter changes within a second, no restart.

6. **A firefight with several bugs, True 60, FPS counter Detailed:** read
   the last two lines.
   - `True 60: 60, late 0/60` with `host` below 15 ms: fine.
   - `True 60: holding 30`: the PC needs more than a VI per frame there. It
     must be a steady `Game 30` (game frame range near 33 ms), not 20s. If
     it reads 20s, `host` will be above about 37 ms: note `host` and `gfx`
     (tasks per frame and ms). A large `gfx` ms means the renderer (the
     RDRAM copies and GPU waits per shadow mask) is the cost.
7. **Audio:** play 10 minutes including the cutscenes. Note when it
   crackles and what the counter shows then (a long `host` time or a
   `holding 30` switch). If the game crashes, send `crash.log`.

8. **Optional A/B, only if runs still differ:** build `main` and this
   branch from the same checkout (`git checkout main`, build, play;
   `git checkout rgo/framerate-fixes`, build, play), five starts each,
   and count the runs that stay at 30. To test whether the input stack is
   involved at all, also build `3b75cfb` (the 60 fps work before the input
   merge; it has no Detailed host line) and count the same way.

If something is off, the support ZIP and `crash.log`, plus a photo of the
Detailed counter at that moment, show which part is wrong.

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
- **Pacing.** RT64 spaces the extra frames with a timer. VIs are paced to
  the audio clock, which is not the display's refresh: on a 60 Hz display a
  frame repeats or drops every few seconds. VSync with the counter's frame
  time range shows it.
- **Fast clock and timing-sensitive code.** The fast clock changes how much
  emulated time CPU work takes. Code that relied on the CPU being slow (a
  DMA finishing before the CPU reads its data, without waiting) would
  misbehave; none showed up from the title to the second level. Original N64
  slowdown On keeps the original timing as a fallback.
- **One-trap MMIO on Windows.** The decoder was measured with GCC's code
  (every access decoded). MSVC may emit other encodings for some accesses;
  those keep single-stepping, which is slower but behaves the same. If
  anything about device timing looks wrong, `JFG_RGO_MMIO_SINGLE_STEP=1`
  goes back to single-stepping every access.
- **RDRAM snapshots.** Every graphics task still copies the changed parts of
  the 4 MB RDRAM image, comparing all of it (and the Windows renderer
  imports it into RT64's copy again). Heavy cutscenes launch about 500 tasks
  a second, so this is now the largest single host cost there. Tracking
  written pages instead would need every native writer (DMA, audio, the
  renderer's writeback) to report its writes; not done yet.
- **One tick per frame:** see the [True 60 audit](#true-60-audit) for what
  was fixed, what was checked and what was left. Watch for anything that
  moves at double speed or not at all at True 60.
- **Half rate.** Bugs, splashes and wakes update 30 times a second at True
  60, as on the console. Anything that reads their state between two
  updates (the player's shots, collisions) sees the same state twice, as it
  would at 30. If something ever looks like it freezes or skips, the table
  is `kHalfRateFunctions` in `frame_interpolation.hpp`; the odd-tick
  compensations are `odd_tick_compensated` there and
  `odd_tick_compensated_call` in `native_boot.cpp`.
- **The governor's busy time** includes everything the emulation thread does
  for a frame, renderer work and audio included, minus only the VI waits. A
  frame at 30 carries two VIs of audio work, so it slightly overstates what
  60 would cost; the 15 ms / 20 ms thresholds leave for that.
- **RDRAM copies per graphics task** are the largest host cost in heavy
  scenes (above). A cheaper import (only the 4 KB pages the CPU wrote since
  the last task) needs every native writer to report its writes and the
  Windows renderer's own writes to RAM tracked; it should be done with the
  renderer in hand, not blind.
