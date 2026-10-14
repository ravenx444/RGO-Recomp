# Accelerated-clock audio DMA repair

The accelerated CPU can fill the original eight-entry PI command queue while
sound streaming requests are being submitted. The supported US audio callback
ignores a failed high-priority read submission and subsequently consumes the
uncopied sample buffer, producing audible scratches despite a healthy host
playback ring.

The runtime now lets this specific audio submission wait for queue space using
the original guest kernel. The guard requires the supported SDK call site,
audio reply queue, high-priority read, accelerated clock and a full eight-entry
PI queue. Other callers, writes, queues and original-clock behavior retain
their existing semantics. Private progress counters record requests, failures
and these waits.

Validation on 2026-10-14:

- The scope test covers the intended case and twelve excluded cases.
- Project validation and native Windows game compilation passed.
- Original-clock 30-second PCM stayed byte-identical to the clean reference.
- Accelerated Original30 and True60 45-second probes exited successfully with
  zero rejected DMA requests, using 25 and 32 guarded waits respectively.
- The owner confirmed the repaired 18-27-second recording sounds clean, then
  confirmed clean live playback through the formerly affected intro section.
- Live testing used a separate launcher folder with copied saves/settings.
  The owner's installed game was preserved. Longer gameplay remains under test.

Recordings, ROMs, generated game code and built game output remain private and
are excluded from source distribution. These checks do not establish a full
campaign pass or resolve unrelated HUD rendering issues.