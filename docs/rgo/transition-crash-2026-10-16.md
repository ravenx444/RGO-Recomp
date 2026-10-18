# Area transition crash investigation, 2026-10-16

Reloading areas containing smart enemies could leave the recompiled pathfinding
module using heap buffers released by an earlier unload. This corrupted unrelated
models, textures, or animation data after the allocator reused those addresses.
Symptoms included intermittent loading crashes and failures shortly after entry.

## Evidence

The owner's reports stopped in model destruction and texture display-list access.
Separate hidden tests using copies of the owner's saves reproduced asset corruption,
including a failure at the same texture access instruction. A focused private watch
then caught pathfinding writing an animation header two video ticks after that
animation acquired the allocation. The pathfinding scratch buffer had been freed
repeatedly since its original allocation without being reacquired.

All 772 newly loaded animation headers retained in that failing run matched the
local ROM at the load boundary. The captured corruption happened afterward.
Private RAM, instruction history, ROM, generated code, and game binaries remain
outside the repository and are not support-report attachments.

## Repair

After the original full-unload routines return, the runtime checks the module's
actual loader entry and retained-allocation list. When its executable base is zero
and no suspended allocation remains, it retires the normalized section as well.
Its next cold publication initializes DATA/BSS before the module initializer or AI
runs, so released scratch pointers cannot survive a full unload.

A suspended module still owns its mutable data. Live, suspended, or malformed
loader state never authorizes retirement. No invalid asset access is skipped or
replaced with dummy data.

## Validation

- Synthetic regression covers a live module, executable suspension with retained
  mutable data, full unload, malformed retained records, reserved slots, and an
  overflowing inventory pointer. Four overlay-related regression tests passed.
- The existing nine controls and audio regression tests passed.
- The first repaired hidden run completed 24,000 video ticks with repeated area
  changes and the corruption watches enabled. Its bounded history recorded five
  full pathfinding unloads, 55 pathfinding allocations, seven frees, no invalid
  repeated pathfinding frees, and no corruption trap.
- The captured failing control recorded 14 invalid repeated pathfinding frees and
  the exact animation overwrite. The test routes are not deterministic, so a clean
  run alone is not treated as proof that every loading crash is resolved.

- A second hidden run using normal instruction batching completed 18,000 video
  ticks and repeated area changes with no PI request rejection. Its history
  recorded five full pathfinding unloads, 30 pathfinding allocations, four frees,
  and no invalid repeated pathfinding free or corruption trap.
- A 4,800-tick input replay with four Start-button presses completed successfully,
  with zero PI request rejections and the final pause flag clear. This is bounded
  runtime evidence, not visual acceptance of every pause-menu screen.
- Final project validation, repository hygiene, and whitespace checks passed.

The separate local `RGO-Transitions-Test` launcher points to the validated native
candidate and uses copies of the prior test saves/settings. It was not launched
on the owner's desktop. Owner gameplay across the reported failing areas remains
pending. This is not full-campaign or public-release acceptance. Working
installations and owner saves are preserved.
