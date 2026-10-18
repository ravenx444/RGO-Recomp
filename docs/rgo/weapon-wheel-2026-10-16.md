# Weapon wheel and Floyd visibility candidate, 2026-10-16

Hold Y / Triangle or Tab during single-player gameplay, point the right stick
or move the mouse, then release to select. The centre cancels. The wheel lists
owned weapons and a Hide Floyd / Show Floyd action when the roaming companion
exists. Movement, firing and camera input are consumed while choosing; the
world continues running. Loading, actor changes, menus, scripted cameras and
focus loss cancel selection. Wheel directions are independent of look inversion.

Selection calls the original US controlSelectWeapon routine, including held
weapon cleanup and clearing aim locks. It also publishes the accepted requested
weapon at player-control +0x585, as the stock cycling path does. A precise
private store watch identified the character controller restoring the old
requested weapon when this second step was omitted. Injected pointers use the
MIPS signed pointer convention, with separate argument-home stack space.

Floyd is identified through sidekickControl and its owner actor. Hiding affects
his model; original companion updates and mission/progression state remain
active. Multiplayer, co-op, scripted cameras and mission piloting take priority
over hiding. Visibility preference lasts for this game session. No game save
or mission flag is edited by the toggle.

The targeting-sound option now covers the two acquisition cue IDs regardless
of caller, including their positional route. Direct playback uses bank IDs,
so that route resolves the current sound tables before matching; unrelated
bank entries remain audible. The sound file can be located through either
the volume or controller profile path. Audible owner acceptance is pending.

Display / Unlocked need Interpolate extra frames enabled to present above the
game's own rate. The settings screen now explains the limit when that option
is off. The private high-refresh candidate starts in Display with extra frames
on, object blending off, and VSync on. Logic remains at up to 60 updates per
second. This change does not establish that combat simulation stays at 60.

## Validation and limits

Seven focused regressions passed: wheel state/cancellation and Floyd context
policy, master volume and effect routing, video settings, frame interpolation,
runlink lifetime, PC input and twin-stick controls. Runtime and launcher built
with the Windows toolchain. Project validation and source/history hygiene were
checked. The wheel was rendered offscreen at maximum inventory size without
interacting with the owner's desktop.

Hidden Windows probes exercised controller Y/right-stick and Tab/mouse through
the real input sampler and original game selector. Floyd hide/show calls and
skipped model drawing were observed. The 4800-VI Display probe selected the
180 Hz target and recorded 9501 presentation events over roughly 83 seconds;
the 3000-VI Unlocked mouse probe recorded 10915 over roughly 53 seconds. These
are hidden-window counters, not a physical display smoothness acceptance test.

The child processes reached their VI limits with exit zero and zero PI request
failures. The outer original-OS evidence wrapper reports acceptance=false
because these runs do not produce its complete original-OS snapshot. They are
bounded gameplay observations, not that evidence gate passing.

The final 4800-VI candidate kept choices 0, 2, 3 and 8 through the original
player update, including repeated selections. Its final private snapshot had
both the equipped inventory slot and requested weapon set to 0. The child
exited zero with no PI rejection; 8993 presentation events were recorded over
83.38 seconds with Display targeting 180 Hz. This verifies persistence rather
than merely a successful selector return. The private checkpoint and candidate
receipt record the matching executable hash. Physical wheel
appearance, audible beep suppression, long campaign play and Floyd mission/
co-op acceptance still need owner testing. The previous transition build is
preserved; the owner completed Vela's entire first level without a crash.
