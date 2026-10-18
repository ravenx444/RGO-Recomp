# Native scene cadence experiment, 2026-10-16

Owner requested actual fresh gameplay above 60 FPS. This work does **not** meet
that acceptance gate yet. Display/Unlocked continue to use the existing normal
Play behavior; no owner fixture or setting was replaced.

## Implemented locally

- Separate an optional extra scene pass from the console VI/Count/audio clocks.
- Account simulation ticks from real elapsed retraces, with reset and six-tick
  stall bounds. More scene passes cannot invent simulation time.
- Restrict the experiment to diagnostics and ordinary single-player gameplay.
  It is explicitly disabled for Play and unavailable through the settings UI.
- Rotate the game's framebuffer bookkeeping for an extra scene while leaving
  the original OS swap and VI/audio interrupts on the real tick boundary.
- Keep zero-tick passes out of the Game simulation-update counter.
- Add Windows frame traces and report the guest address of a fatal BREAK.

The first active experiment trapped at normalized PC `00F01B50`, inside Vela's
`girlControl`: displacement is divided by the integer tick argument. Object
simulation now runs only when there are elapsed device ticks. Rendering still
executes between those updates. This is scaffolding for separating the two
loops, not fractional actor simulation and not proof of smoother movement.

## Bounded evidence

Private local evidence and binaries are under the owner's Modding folder:
`RGO-Audio-Repair-Test/native-fps-investigation-20261016/CHECKPOINT.json`.
The final isolated runtime is `native-fps-candidate-v18/jfg-native-boot.exe`.
Nothing was uploaded.

Both comparison runs use a copied profile, hidden graphics, neutral virtual
input, and dummy audio. They include startup and 3600 real retraces, about a
minute. Normal gameplay measurements use trace rows after retrace 2100.
The control and candidate are separate sequential runs, not a performance
claim for another machine or a tightly deterministic scene replay.

| Observation | Control | Final experiment |
| --- | ---: | ---: |
| Scene-loop entries per second | 45.13 | 51.65 |
| Simulated device ticks per second | 59.78 | 59.70 |
| Zero-tick scene passes in measured interval | 0 | 486 |
| Mean host busy time per scene-loop entry | 18.99 ms | 17.77 ms |

These scene-loop entries are not all distinct moving-world images. In the
experiment, whole-tick actor simulation remains below 60 updates per second
in this loaded test. The tick budget preserves elapsed time but does not make
motion fractional. Counting these entries as >60 gameplay would be incorrect.
The existing Render/Display counter definitions were not changed.

Both final child processes reached the requested retrace target and exited 0,
with zero unsupported accesses and zero PI request failures. The outer probe
wrapper still returns 3 because original-OS acceptance is false and the full
runtime snapshot gate is incomplete. This is bounded runtime evidence, not
release acceptance. Dummy audio does not validate audible quality.

Native compilation, focused frame-interpolation/tick-budget tests, project
validation, repository hygiene, and whitespace checks passed. Earlier failed
candidates and their traces remain preserved locally.

## Remaining implementation

1. Measure game-update, scene-draw, camera, snapshot, and guest-dispatch CPU
   costs separately. The observed roughly 18 ms host cost also prevents high
   frame rates in this fixture, regardless of the retrace wait.
2. Convert motion/camera/animation paths to fractional elapsed time while
   keeping integer timers, AI scheduling, collisions, audio, and input edges
   coherent. Audit all three characters and water/boss states.
3. Handle menus, cutscenes, loading, auxiliary graphics tasks, and buffer
   ownership before enabling the new cadence outside diagnostics.
4. Require distinct moving scenes above 60, normal-speed movement and sound,
   prolonged combat and transition tests, and owner visual confirmation.

The pending boss/Tri-Rocket crash capture remains a separate unresolved issue.

## Work profiling follow-up

A separate `native-fps-candidate-v19` adds an optional Windows-only
`JFG_DIAGNOSTIC_WORK_TRACE` TSV. It measures inclusive wall duration and host
thread cycles for the frame loop, main update, object update, front/HUD update,
and camera update. Thread cycles exclude time when that guest host thread is
asleep; entries nest and must not be summed as independent costs. The trace
is disabled for normal Play.

Heavy progress reports and private snapshots were removed for a second pair
of bounded runs. The control reached 57.50 scene-loop entries/s at 59.81 device
ticks/s; the experiment reached 61.06 entries/s at 59.78 ticks/s. Of the latter
1530 entries, 701 had zero ticks. This modestly exceeds 60 loop entries, but
**does not establish fresh actor simulation above 60**. Both child runs reached
3600 retraces and exited 0. Neither acquired full original-OS acceptance.

The lean control's inclusive mean wall durations were:

| Routine | Mean wall time |
| --- | ---: |
| Object update | 8.454 ms |
| Main game update, including objects | 9.350 ms |
| Front/HUD update | 0.166 ms |
| Camera update | 0.016 ms |
| Whole frame, including VI waits | 17.378 ms |

The earlier full diagnostic task trace also measured roughly 6.3 ms per main
submission and 2.8 ms per auxiliary submission, mostly in full sync/readback.
These are observations from a different run, not additive costs from the table.
The game's CPU pixel processing depends on returned color/intensity buffers;
disabling writeback without replacing those consumers would break effects.
The next optimization should target those ownership/synchronization boundaries
and object update work, rather than camera math or increasing a display cap.


## Private v20 through v22 iteration

The CPU command decoder now caches its renderer context by State pointer and a
registry revision, invalidated on registration and removal. Multi-context,
destruction, replacement, texture-output, and lookup-count tests pass. A
200,000-command CPU microbenchmark measured 3,958 us without the cache and
1,793 us with it in the latest run. This is not a whole-game FPS measurement.

The opt-in v21 diagnostic presents a completed extra scene framebuffer through
the normal framebuffer checks without manufacturing another console VI or audio
interrupt. It disables RT64 frame/object interpolation in the diagnostic copy
of video settings. In a 3,600-retrace hidden run, steady native delivery reached
87.53 frames/s while simulation remained 59.79 ticks/s. Zero-tick scene passes
remain, so this does not demonstrate actor motion above 60. The native child
exited 0; the wrapper returned 3 because original-OS acceptance and complete
runtime snapshot conditions were not met.

V22 builds with a diagnostic cache-off comparison switch and cumulative
scene/camera change counters. Its runtime comparison is pending. Focused shell,
texture/context, and frame-interpolation tests passed, as did project validation,
repository hygiene, and the whitespace check. The owner boss crash and a
repeatable room-entry fixture took priority before the v22 runtime comparison.
Ordinary Play does not enable the experimental scene/presentation flags.


## Native fractional player prototype, v23 through v27

The native experiment now supports player movement between whole object ticks.
It is disabled in ordinary Play and has not been promoted to an owner fixture.
Only Vela's normal ground movement has live evidence. Juno and Lupus have
reviewed entry and damping hooks, but no live fractional validation yet.

The first attempt changed floating movement time while preserving integer
updates. It moved Vela on intermediate frames but reduced measured movement
speed by roughly half. Ground locomotion rounds elapsed time to an integer
before its damping powers. Twelve reviewed damping calls across the three
characters now preserve the fractional exponent. Other powers retain their
existing integer semantics. Three final sound turn-rate divisions also retain
a nonzero denominator on explicitly requested zero-tick player updates.
This does not establish audible output quality.

V26/V27 use CPU Count divided by the actual VI period for fractional player
time. Integer object ticks and audio stay on the console clock. Native scene
batches are bounded to three intermediate opportunities. Controller diagnostic
scripts can now hold inputs by retrace count instead of poll count, so increased
scene cadence cannot shorten the test's input durations.

The final V27 observations after retrace 2100 were:

| Observation | Low-resolution control | Low-resolution fractional | Owner settings fractional |
| --- | ---: | ---: | ---: |
| Player position changes per second | 45.98 | 78.18 | 77.38 |
| Changed scene frames per second | 70.53 | 67.93 | 67.76 |
| Total native presentations per second | 111.29 | 105.95 | 104.70 |
| Fractional elapsed ticks per second | 60.00 | 60.01 | 60.01 |
| Mean horizontal speed, game units/second | 47.91 | 49.23 | 48.86 |

The control still generates extra scenes and uses integer object/player steps.
Scene changes include camera motion and cannot establish fresh enemy animation
or world simulation at that rate. Presentations include repeats. The roughly
3 percent speed/path difference still needs calibration and deterministic
analysis; this is not a gameplay fidelity pass. All runs used copied saves,
a hidden window and virtual input. No desktop input was sent. Owner video
settings were copied, not modified.

A separate V27 owner-settings work profile observed 79.71 position changes/s
and 66.47 changed scenes/s. Mean inclusive wall durations were 12.57 ms for the
whole frame, 1.23 ms for zero-tick object entry/player-only updates, 3.15 ms
for one-tick objects, and 7.84 ms for two-tick objects. The whole-frame duration
includes waits and nested work; these costs must not be added together or
interpreted entirely as GPU time. Earlier task-level evidence still identifies
full-sync/readback waits as a performance target. Required CPU pixel consumers
must be preserved when changing those boundaries.

V27 native compilation, 237 controller checks, frame/clock/damping/cadence tests,
five original timing generation tests, project validation, repository hygiene
and whitespace checks passed. Each bounded run reached 3600 retraces, native
child exit 0 and zero unsupported accesses. The outer wrapper still returned 3
with acceptance false and runtime_snapshot_complete false. Dummy audio does
not establish audible quality. Combat, other movement states, both other
characters, modern look hooks, transitions and a 180 Hz target remain open.

Resumable private results, runtime hashes, source backups and the work profile
are recorded in the local native-fps-investigation-20261016 checkpoint. Previous
owner builds and the separate boss-room test fixture remain untouched.
