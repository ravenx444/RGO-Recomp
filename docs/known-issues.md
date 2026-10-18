# Known issues

Check this page before opening a report. Report RGO problems at
[RGO issues](https://github.com/ravenx444/RGO-Recomp/issues), with the
launcher's support ZIP.

## RGO

- In **True 60**, the heaviest cutscene shots fall back to 30 fps, and big
  firefights can dip below 60.
- First-time setup takes about an hour (installing tools and building the
  game from your ROM). It has not crashed; leave the launcher open.
- The launcher's folder must have a short path (for example `C:\Games\RGO`),
  because the build hits Windows path-length limits; the launcher says so
  before setup starts.

The sections below come from upstream JetForceGemini-Recomp and still apply
(links to issues and releases point at upstream). Upstream's
[GitHub issues](https://github.com/TK22-26/JetForceGemini-Recomp/issues)
contain the ongoing discussion there.

## Windows setup

RGO setup runs directly on Windows. WSL, Ubuntu and BIOS virtualization are not
required. Older upstream WSL setup reports do not describe this package.
A fresh install can still be blocked by Windows Application Control rejecting
an unsigned locally compiled generator. Preserve the setup logs for administrator
or security-provider review. Clean-machine setup remains unqualified.

The tester ZIP includes `TESTING.md` with current gameplay validation limits.
The newest loading safeguard requires further owner and tester confirmation.

## Release and source versions

Each released launcher builds its pinned source revision. `main` can contain
newer gameplay fixes, so include your launcher version and game/source build
when reporting. See [getting started](getting-started.md) for both paths and
[recent fixes](development/boot-gameplay-fix.md) for their validation scope.

## Controller coverage

RGO includes native DualShock 4 and DualSense support, gyro aiming on supported
controllers, direct mouse aiming, and Modern twin-stick controls. See the
[RGO input notes](rgo/input.md) for supported devices, settings, and owner test
steps. These are RGO features; upstream's older XInput-only limitations do not
describe this fork.

The Modern controls remain bounded by the game's camera behavior. Fully free
camera control and crosshair-to-shot alignment are still planned. Coverage
across additional controllers and multiplayer modes needs further testing.

## Gameplay coverage

Upstream's maintainer-reported progress was tracked in its README, which this fork removed.
The maintainer has completed the campaign using optional infinite-health and
enemy auto-kill testing mods and a save with all Tribals unlocked. Saving and
loading are also verified. This establishes campaign progression coverage;
original-console accuracy and broader unassisted play coverage remain under
validation. Report a specific failure with its location and reproduction steps using the
[playtesting guide](playtesting.md).

## Player shadow darkness

The native renderer retained a raw player silhouette because its offscreen
intensity buffer was not returned to game memory. The game's CPU blur and
opacity reduction therefore read an empty buffer. Preview 0.4.0-preview.3 builds
return these masks before task completion and preserve subsequent CPU edits.
A copied-save Juno test in the tutorial confirmed the processed opacity range
against Angrylion and exited cleanly. See [the renderer fix and validation
scope](development/boot-gameplay-fix.md#player-shadow-cpu-postprocessing).
Older builds need rebuilding; full-campaign shadow accuracy remains unverified.

## Particle textures

Preview.4 fixes stale renderer memory that produced opaque
particle rectangles. The recorded Vela water splash has before/after replay
validation. The maintainer also confirms that all previously reported broken
particle effects now render correctly, including dust, pickup sparkles, ship
exhaust and Tawfret rain. Juno's water splash has not been tested.
See [the renderer evidence](development/boot-gameplay-fix.md#cpu-texture-refresh-after-framebuffer-reuse).

## Support reports

Preview 0.4.0-preview.2 supports session selection, filtered system/build details,
crash stacks and an on-demand freeze snapshot. Rebuild an older game to gain the
native capture features. Capture remains best effort: forced termination, early
startup failures, corrupted stacks or access restrictions can leave partial logs.
These are text stack snapshots, not full-memory dumps. See [playtesting](playtesting.md)
for the file inventory and attachment instructions. Nothing uploads automatically.

## Experimental navigation mod

Preview 0.4.0-preview.3 includes an opt-in Navigation mod with a live height map,
item/NPC markers, a character inventory tracker, and shared walking/jump routing.
NPC reward dialogue, item-room returns and door requirements have focused tests.
The unified stair/NPC route has recorded-geometry and simulated-command coverage;
a full native run of that sequence and autonomous campaign completion remain
unverified. Lupus hover, moving platforms, shooting/explosive gates and unsupported
scripted traversal still need manual play. Normal gameplay leaves the mod off.
See [navigation validation and limits](development/navigation-mod.md).
