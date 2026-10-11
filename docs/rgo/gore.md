# Persistent gore (roadmap Tier 1, item 7)

Goal: bodies, splatter and especially the collectible alien heads don't
vanish. Drone heads must stay until the player picks them up.

**Status.** Severed limbs, drone heads (and the other pickups that drop as
limbs) and blood splats on floors and walls stay. Each has its own setting
under **Game > Gameplay settings...** and a limit; past the limit the oldest
fades exactly as in the original game. Dead bodies themselves still go (see
[Bodies](#bodies-not-changed)). Measured headlessly: see
[Headless check](#headless-check). Owner test steps are at the end.

## What the game does

All addresses are the US version.

### Severed limbs and drone heads

Every limb that comes off a shot enemy, including the drone's head, is an
object driven by `severedLimbControl` (0x80027844, `spranim.c`, still ASM in
the decomp). Its state (object + 0x68) is set up by `severedLimbInit`
(0x800275E8):

| Offset | Type | Meaning |
|---|---|---|
| +0x14 | s16 | fade timer, 950 at spawn |
| +0x17 | u8 | kind (from the object definition) |
| +0x18..+0x1C | s16 x3 | spin |
| +0x1E | u8 | 0 if the owner was a Tribal, else 1 |

Each frame, with `updateRate` the frame's 60 Hz ticks (2 at 30 fps, 1 at
True 60):

1. While the timer is above `updateRate` it drops by `2 * updateRate`, so
   2 per tick: 950 lasts about 7.9 s at either frame rate.
2. Then the object's alpha (object + 0x39) drops by `2 * updateRate` per
   frame (about 2.1 s from full), and `objFreeObject` frees it.
3. Kinds 2 and above can be picked up (`hitPlayer`, 40 units):
   - kind 2 is a **drone head**: it adds one to the character's head count
     (character + 8, or + 6 for a Tribal's), saves global flags at 100, 200
     and 300 heads (the unlocks), plays ditty 13 and frees the object;
   - kind 3 gives ammo for the character's weapon, kind 4 weapon 6.

So in the original game a head lies there for about 10 s in total.

### Blood

Overlay 8 (`blood.c`) keeps a fixed pool of **64 blood spurts** in its BSS
(BSS + 0x20, 0x54 bytes each). `bloodSpurtAdd` (overlay 8 + 0x1CC) takes a
free slot and **silently drops the new blood when all 64 are in use**.
`bloodSpurtUpdateAll` (overlay 8 + 0x40C) moves each one:

| State (+0x4A) | Meaning |
|---|---|
| 1 | flying under gravity; on hitting the level it becomes a splat |
| 2 | splat growing to its size |
| 3 | splat at rest; +0x3C counts down from 300 ticks (5 s) by `updateRate` |

`bloodSpurtsDraw` fades a resting splat over its last 120 ticks (2 s) and
only draws splats that landed on a surface (+0x52). There are also 5 short
hit sprites (`bloodHitSpriteAdd`), animated flashes that end on their own.

### Memory and object limits

- Limbs live in the main memory pool: the object, plus 3-4 allocations for
  the limb's own vertices and triangles (`severedLimbInit`,
  `modMakeLimbModel`). The main pool has **1600 allocation slots**
  (`mmInit`). `FreeRAM` (0x800FDD90) holds its free bytes; below 80 KiB
  (0x14000) `mmFreeTick` starts unloading overlays (`runlinkLowMemoryPanic`).
  The US game calls `sparkUpdate` where `squadsLowMemoryPanic` was meant, so
  there is no further fallback.
- The object list (`ObjList`, 0x800F2CA4) has **no overflow check** in the
  retail build (the "ObjList Overflow" message is never printed). Measured
  headlessly, its allocation is 2048 bytes: room for 512 objects. The opening
  cutscenes and the first areas use 20-60.
- Measured main-pool use: about 1.2 MB free and 343 of 1600 slots at the
  title, about 630 KB free and 620-650 slots in Goldwood's first area.

## What RGO changes

`gore_dispatch` in `src/boot/native_boot.cpp`; constants, settings format and
bookkeeping in `include/jfg/rgo/persistent_gore.hpp` (unit tests in
`tests/rgo_gore_tests.cpp`). Nothing of the game is replaced: the hook only
adjusts timers before the game's own function runs.

- **Limbs and heads.** On each `severedLimbControl` call, a kept limb whose
  timer has fallen below 120 gets 120 again, so the game never reaches the
  fade. Heads and pickups (kind 2 and above) and plain limbs (kinds 0 and 1)
  are counted separately, in the order they were first seen. The defaults
  keep **64 heads** and **48 limbs**; one more lets the oldest go, and it then
  runs its last 120 (1 s) and fades as in the original. A head that is
  picked up, faded out or unloaded is forgotten (`objFreeObject`, 0x800079B8);
  `objFreeAll` (0x80004838) and `mainChangeLevel` (0x8004665C) clear the list.
- **Blood.** On each `bloodSpurtUpdateAll` call, resting splats that landed
  on a surface keep their timer at 128 or more (full colour). The default
  keeps **40** of the 64 slots, so 24 are always free for new blood; past
  40 the oldest splat fades out over 2 s. The pool is found where the
  generated code keeps it: overlay 8's normalized address
  (`section_addresses[8]`, a window above RDRAM while the runtime has the
  overlay active) plus its text and data sizes, checked against its BSS size
  and `bloodSpurtUpdateAll`'s first two instructions. (The game's own runlink
  table also records a load address in RDRAM, but the recompiled code does
  not use that copy; it stays zero.) If anything does not match, blood is
  left alone.
- **Low memory.** Before keeping a limb, the hook reads `FreeRAM` and the
  main pool's used slots. Below 256 KiB free or 256 free slots it lets the
  oldest kept limb go (plain limbs first, then heads), at most one per game
  frame. That is far above the 80 KiB where the game starts unloading
  overlays, so kept gore can never starve new enemies or projectiles.
- **Frame rate.** Every timer here is in 60 Hz ticks scaled by the game's
  `updateRate`, and the hook only sets a floor, so Original and True 60 keep
  things equally long. The limits count objects, not time.
- **Replays and captures** keep the original behaviour (deterministic input
  replays).
- These functions are not touched by the True 60 work
  (`docs/rgo/framerate.md`).

### Settings

`gore.ini` in the game profile, next to `video.ini`; the game rereads it
every 250 ms, so changes apply while playing. A missing file means the
defaults (everything stays).

```
version=1
heads=on
limbs=on
blood=on
head_limit=64
limb_limit=48
blood_limit=40
```

The launcher's **Game > Gameplay settings...** page switches the three on
and off. The limits can be edited in the file (heads and limbs up to 128,
blood up to 48).

## Bodies (not changed)

A dead drone's body is the squaddie object itself (overlay 3, the squad AI),
not a separate corpse object. `squadsKillSquaddie` (overlay 3 + 0x9FF0)
drops the squadron's bonus item once the whole squadron is dead and then
calls `RemoveSquaddie` (overlay 3 + 0x9A14), which takes it out of its
squad. Keeping that object around would mean holding up that path (bonus
items, doors opened by a cleared squadron) and keeping the AI running on a
dead body. Keeping bodies properly needs a separate static copy of the dead
body (model, last pose, position) drawn in its place; that is a bigger
change, left for a later PR. The limbs and the head that come off it do
stay.

## Headless check

Headless Linux build (`docs/rgo/framerate.md`, "Headless measurement") of
`rgo/next` with this branch merged, True 60, the new-game autopilot, plus a
temporary trace (not committed) of the blood pool and the object and memory
counts.

- **Blood.** The opening cutscenes add 137 blood spurts.
  - Gore off: every resting splat counts 300 -> 0 and frees, as in the
    original.
  - Gore on: resting splats stop at 126 and keep full colour. At most 40 are
    kept; extra ones are let go and fade out.
  - The pool fills completely during the heaviest burst in both cases (64
    spurts in flight at once), so keeping splats does not drop more new blood
    than the original game does.
- **Pool location.** The first version used the address in the game's runlink
  table; that copy of overlay 8's BSS stays all zero, because generated code
  uses `section_addresses[8]`. Fixed and re-measured as above.
- **Limbs and heads.** Not exercised headlessly: the autopilot never reaches
  an enemy (also with the navigation mod's instant kill on, no squad member
  came near in the first areas). Checked instead that `severedLimbControl`'s
  and `objFreeObject`'s first two instructions are in RDRAM as expected, so
  the hook qualifies, and the bookkeeping is unit tested. The owner test
  covers the rest.

## Owner test

1. **Heads.** In Goldwood (Juno's first level), kill drones until a head
   comes off. Leave it, wait at least 30 s: it must still be there. Walk over
   it: it is collected.
2. **Limbs and blood.** Severed limbs and splats on floors and walls must
   still be there after 30 s.
3. **Settings.** Game > Gameplay settings...: switching a setting Off while
   playing makes the kept items fade within about 3 s.
4. Repeat 1 at Frame rate **True 60** and **Original**.
5. A long fight (30+ drones) in one area: new enemies, shots and blood keep
   appearing; no slowdown or crash.
