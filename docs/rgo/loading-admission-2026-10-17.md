# Loading request admission repair, October 17, 2026

The owner captured a loading hang in the crosshair candidate after clearing a
room and entering the next area. The main guest thread was waiting on the asset
completion queue, the PI manager was idle with an empty command queue, and no PI
transfer or interrupt was pending. The runtime recorded one rejected PI request,
a maximum command queue depth of eight, and 529 observations of a full queue.
The game frame stopped while video interrupts continued. Native stack capture
and local logs were preserved privately without moving the owner window.

## Repair

The captured state is consistent with a lost asset request. Source inspection
identified an admission race: the existing accelerated-clock safeguards allowed the original kernel to wait
only when the host observed a full queue at function entry. That observation
precedes the kernel disabling guest interrupts. A producer can fill the last
slot during this gap, causing the original nonblocking send to return failure.
The asset reader ignores that return value and then waits for a completion that
will never arrive. The supported audio streaming path also ignores rejection.

Both narrowly identified read paths now permit blocking at every valid queue
depth, including a not-yet-full snapshot. The original kernel still returns
immediately when space is available and owns admission, sleeping, and wakeup.
The caller, command queue, reply queue, read type, priority, capacity, original
nonblocking flag, and accelerated-clock checks are retained. An invalid depth
above capacity remains outside the safeguard. Other queue sends keep their
original behavior. Congestion counters retain their full-snapshot meaning.

A bounded support event records the first eight rejected requests with caller,
reply queue, and video tick, making future failures diagnosable without a full
memory capture.

## Validation

Focused compile-time and executable regressions cover every valid snapshot
and every possible subsequent queue depth, including the seven-to-eight race.
They also retain the caller and queue scope checks and reject invalid depths.
The previous full-only policy rejects that modeled race. Windows Release
candidate compilation, focused tests, project validation, repository hygiene,
and whitespace checks passed.

A hidden run against a copy of the current owner profile completed 12,000 video
ticks and about 93,000 PI requests without rejection. That run did not cross
rooms and is treated as streaming evidence only. A separate private fixture
using the original level-change routine completed 12 room changes between areas
42 and 53 over 12,000 video ticks without rejected requests or a loading stall,
with normal instruction batching and frame interpolation enabled.

These are bounded component and loading tests, not full-campaign acceptance.
The probe child reached its video-tick limit with exit zero; the wrapper's
original-OS snapshot acceptance remains false and is not reported as a passing
full-system gate. The previous-policy normal-batching comparison also completed its 12 room
changes without rejection, so that route does not independently reproduce the
owner failure. Comparison and scheduling-pressure results are stored in the
private local evidence report. ROM, generated code, memory, saves and binaries
are not repository content.
