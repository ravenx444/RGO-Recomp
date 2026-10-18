# Wheel controls and gameplay candidate, 2026-10-16

The previous owner-tested runtime and profile remain in RGO-WeaponWheel-Test.
The revised local fixture is RGO-Wheel-Controls-Test. These are local test
artifacts, not a public release or an instruction to rebuild from GitHub.

## Changes

- Player 1 Controls exposes a learned controller wheel binding, a separate
  learned keyboard/mouse binding, and left/right navigation stick selection.
  The defaults are Y/Triangle, Tab and the left stick. Companion controllers
  can also learn the wheel binding when keyboard is the primary device.
- The wheel uses the game's 15 menu weapon icons. The existing verified-ROM
  decoder exports them into the local profile. No game pixels ship in source
  or launcher resources. Missing icons fall back to text.
- Floyd is outside the radial choices. While the wheel is open, a rising press
  of the mapped game A/interact action requests hide/show. Holding interact
  does not repeat the action. Gameplay/mission visibility restrictions remain.
- Menu D-pad navigation preserves the original D-pad button bits, which the
  original weapon-shortcut editor requires in addition to analog navigation.
- Target acquisition Off now suppresses cue 29 only at the two enemy-reticle
  acquisition call sites, return addresses 8007C0D8 and 8007C1D8. The previous
  1E4/1E5 character lock-on voice suppression was the wrong sound and is removed.
- The Start-menu frame animator receives three times its ordinary elapsed
  ticks while paused. Other menu input, gameplay time and front-end timing
  retain their existing rates. Completion still runs through the original
  animator. Unexpected elapsed arguments are unchanged.
- Normal-follow camera input is allowed in locomotion states 0 through 4 for
  all three characters. The original camera dispatch, cutscene exclusions and
  special-state restrictions still apply. Actual surface/underwater play needs
  owner verification; this gate change alone is not that verification.

## Boss crash evidence and containment

The owner's 2026-10-17T01:12:00Z report identifies runtime SHA256
773b659c8b43fb2f6a90d3086537d6755cdd59a7fc8f34f1da99e18dffa8d92d.
Resolving its native stack against that runtime's matching PDB locates the
fault at GalaxianBatsSoundsHandler, normalized guest PC 08F024B8, loading a
control pointer from an invalid bat-list entry. The recorded access offset
80000067 is consistent with the linked entry FFFFFFFF plus offset 68.
The generic guest PC recorded in the report was stale; the matching native
stack and generated line identify the actual failing instruction.

The candidate validates this optional ambient-sound chain before invoking the
original sound selector. An invalid pointer, missing state or cycle skips
only that ambient callback and records boss=invalid-ambient-sound-chain once.
Valid chains retain the original random selection and playback. It does not
rewrite the list, enemy state or combat. The cause of the invalid list remains
unresolved; do not describe containment as a proven lifecycle repair.

## Validation and open checks

Five focused C++ regression executables passed, including legacy input-profile
migration, wheel action edge handling, cue/call-site isolation, camera gates,
menu timing and malformed/cyclic boss ambient lists. Windows runtime and
launcher builds passed. The managed launcher suite passed 568 checks and setup
passed 17 checks. Project validation, repository hygiene and whitespace passed.

The 15 real icons were exported locally and the complete radial layout was
rendered and inspected offscreen. Separate hidden 4800-VI probes observed
default left-stick selection, custom opener/right-stick selection, keyboard
selection and pause open/close without child crashes or PI request failures.
The final candidate records menu=fast-pause-animation on the original animator.
The existing diagnostic wrapper rejects these otherwise clean bounded runs
because its original-OS snapshot gate is incomplete. Child exit zero and the
observed behavior are not a claim that this broader acceptance gate passed.

Still requires owner play: shortcut assignment/use in all four directions,
actual audible beep Off/On, Floyd with a live companion, swimming camera, and
another boss fight. Tri-Rockets/Remote Mines crosshair flicker/disappearance
remains unresolved. No public upload or release was performed.

## Follow-up: rocket hit on spawned bats

The owner identified the first swamp boss as Juno and then reproduced another
crash immediately when a Tri-Rocket hit spawned bats. The v12 log confirms
that the optional ambient guard detected an invalid chain before this crash.
The new native stack resolves to squadsCheckEnemyPointers, guest PC 80051628,
while object cleanup reads a bat's control pointer. This proves the earlier
sound-only containment is insufficient. The invalid shared list still needs
its producer/lifetime repaired; no boss-completion acceptance is claimed.

Private diagnostic builds entered level 195 as character 1 through the original
level-change routine and equipped weapon 5. Controlled runs, including a
Floyd-hidden run and an explicitly invulnerable copied-save fixture, reached
7500 VI without reproducing the malformed bat chain. These are not evidence
that the normal-play failure is fixed. All temporary destination/health test
setup stays in private fixtures; forced travel instrumentation was removed
from the pending source after each diagnostic build.

A separate concrete reticle defect was corrected: frontDrawTarget was included
in the HUD bounds classifier and could acquire a left/right screen anchor when
its projected geometry moved into an outer third. It now always preserves
camera coordinates. Regression cases cover geometry at both edges. Visual
acceptance of the reported Tri-Rocket/Remote Mine symptom remains pending.

The isolated RGO-Boss-Capture-Test fixture pairs runtime v13 with the current
launcher. Capture Boss.cmd explicitly enables private first-invalid-chain and
crash memory captures plus bounded gameplay tracing under private-captures.
Ordinary launching does not enable these captures. No forced travel or player
health modifications are present in that runtime. Six focused regressions,
project validation, source hygiene, and a bounded 2400-VI child run passed;
the original-OS wrapper snapshot gate remains incomplete as described above.

The owner also confirmed Display around 160 on a 180 Hz monitor while Game and
Render stay at 60. High-rate presentation is functioning, but native scene
production above 60 and combat drops below 60 remain unresolved. Raising the
interpolation target or renaming counters is not acceptance of that request.

## Crosshair follow-up, 2026-10-17

The missing Tri-Rocket and Remote Mine sights were traced to CPU framebuffer
changes being omitted from RT64's separate interpolated presentation targets.
A renderer compatibility patch restores the original sights and digits there.
Before/after interior-frame captures and compatibility checks are recorded in
[crosshair repair](crosshair-repair-2026-10-17.md). Sustained owner combat and
weapon-switching validation remain pending; the earlier unresolved entries
above describe the previous candidates.
