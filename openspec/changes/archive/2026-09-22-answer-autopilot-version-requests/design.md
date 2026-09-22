## Context

See `proposal.md` — *Why* for the motivation. This section only adds the facts needed to
justify the approach.

`src/mavlink.cpp`'s outbound path is a tagged union, `LinkMsg` (`include/LinkMsg.h`): a
producer fills one on its own stack, `xQueueSend`s it by value onto the port's write queue
(depth 6, non-blocking, drop-on-full — there is no producer/consumer margin, by design),
and `TaskLinkWrite` receives it and calls `mavlinkPack()` to turn it into a
`mavlink_message_t`. `sizeof(LinkMsg)` is pinned at 64 bytes by `statustext`'s 50-byte text
field (`include/LinkMsg.h`'s own `static_assert`). `LinkMsgKind::Heartbeat` already
demonstrates that a variant can carry **no** union payload when nothing it needs is runtime
state: `mavlinkPack()` re-reads `reducedConfiguration` and calls `packCustomMode()` itself,
rather than the producer snapshotting them into the queue item.

`TaskMavlink`'s `COMMAND_LONG` switch already has two live sub-commands —
`MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` and `MAV_CMD_SET_MESSAGE_INTERVAL` — each of which
always replies with `COMMAND_ACK`, and each of which is a single `sendX(port)` /
`xQueueSend` call, the shape every other `send*` function in the file follows.

Its outbound write queue (`uartWriteQueueStorage` / `usbWriteQueueStorage`, depth 6 each,
`src/main.cpp`) was sized against the periodic schedule's four independently-clocked
entries — `HEARTBEAT`, `SYSTEM_TIME`, `BATTERY_STATUS`, housekeeping's `NAMED_VALUE_INT` —
coinciding on the same pass (the reasoning `add-mavlink-housekeeping-telemetry`'s design.md
used to grow the queue). There is no producer/consumer margin beyond that.

## Goals / Non-Goals

**Goals:** answer `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` truthfully, with no new RAM cost
in the outbound queue and no new runtime state.

**Non-goals:** this does not implement any of the protocols `AUTOPILOT_VERSION` could in
principle advertise (mission, parameter, FTP) — it reports their absence. It does not add a
firmware version scheme, a board revision id, or a vendor/product id; none exists today and
inventing one is a separate decision this change does not make. It does not touch
`REQUEST_DATA_STREAM` or `PARAM_REQUEST_LIST`, both `TODO.md`'s concern under different
entries.

## Decisions

**1. `AUTOPILOT_VERSION` carries no `LinkMsg` payload — `kind` alone is the whole message,
same as `Heartbeat`.**
Every field this firmware can honestly fill is a compile-time constant: `capabilities` is
`MAV_PROTOCOL_CAPABILITY_MAVLINK2` and nothing else (Decision 2), and every version/vendor/uid
field is `0` because nothing tracks one. None of that is runtime state a producer needs to capture at
enqueue time, so `mavlinkPack()`'s new `case LinkMsgKind::AutopilotVersion:` builds the
whole message itself, the same way it already rebuilds `HEARTBEAT` from
`reducedConfiguration` rather than from a queued snapshot.
*Alternative considered:* add a payload variant anyway, for symmetry with the other six
kinds or against a future where `capabilities` becomes non-zero. Rejected — if a later
change implements a real protocol, which bits are set is still a build-time fact (this
firmware is not reconfigurable at runtime; see `memory-budget`'s constraints), so it is
still a constant `mavlinkPack()` can compute, not a value that needs to travel through a
queue. Carrying an unused payload field today is exactly the kind of thing this project's
rules tell a proposal not to do without a present reason.

**1a. `AUTOPILOT_VERSION` and `MAV_PROTOCOL_CAPABILITY` are not reachable from `<MAVLink.h>`
as this project already includes it — found while implementing task 1.2, not anticipated
when this design was first written.**
`src/mavlink.cpp` includes `<MAVLink.h>`, which resolves to `mavlink/common/mavlink.h` →
`common/common.h` — the vendored **common** dialect. `common.xml` itself documents why:
line 6933 carries the comment `<!-- <message id="148" name="AUTOPILOT_VERSION"> in
standard.xml -->` — the message was moved to the included `standard.xml`, and this
vendored copy's `common/common.h` was never regenerated to pull the per-message header in:
`common/` has no `mavlink_msg_autopilot_version.h` at all, and `common.h`'s own
`MAVLINK_MESSAGE_INFO` aggregate macro references `MAVLINK_MESSAGE_INFO_AUTOPILOT_VERSION`
without ever defining it — a pre-existing dangling reference in the vendored library, inert
only because nothing in this project (or, apparently, upstream's own generation for this
dialect) ever expands that macro. The `MAV_PROTOCOL_CAPABILITY` enum has the same gap: it
is not in `common/common.h` at all, only in `mavlink/standard/standard.h`.
Fix: `#include <mavlink/standard/mavlink_msg_autopilot_version.h>` directly. It is
`#pragma once`-guarded, self-contained (`<stdint.h>` plus the shared, dialect-agnostic
`mavlink_helpers.h`/`protocol.h` at the `mavlink/` root that `common.h` already pulled in),
and defines nothing `common/` already defines, so there is no redefinition risk. Including
the whole `mavlink/standard/standard.h` instead was rejected: it is a full second dialect
that independently re-includes per-message headers for messages `common/` already defines
(`HEARTBEAT`, `COMMAND_ACK`, ...) from its own `standard/` copies — different file paths,
so `#pragma once` does not deduplicate them, and the build would fail on redefinition.
For `MAV_PROTOCOL_CAPABILITY_MAVLINK2` there is no equivalently narrow header — MAVLink's
generator bundles enums into the dialect's top-level file, not one per constant — so the
value (`8192`, read directly from `mavlink/standard/standard.h`) is a local named constant
in `src/mavlink.cpp` with a comment citing where it is defined and why it is not `#include`d.

**2. `capabilities` is `MAV_PROTOCOL_CAPABILITY_MAVLINK2`, and only that bit.**
Verified by reading the vendored library rather than assumed from its default behaviour.
`mavlink_get_channel_status(chan)` returns a pointer into `static mavlink_status_t
m_mavlink_status[MAVLINK_COMM_NUM_BUFFERS]` (`mavlink/protocol.h`) — zero-initialised
static storage, so `flags` starts at `0` on both channels, and `MAVLINK_STATUS_FLAG_OUT_MAVLINK1`
(`mavlink_types.h`, value `2`) is therefore clear at boot. Every `mavlink_msg_*_pack_chan()`
call in this file resolves to `mavlink_finalize_message_buffer()`, which reads exactly that
bit to choose v1 or v2 framing (`mavlink_helpers.h`). Nothing in `src/`, `lib/` or
`include/` calls `mavlink_set_proto_version()` — the only function that touches it — so the
bit never changes. The one other write to that same per-channel struct,
`src/link.cpp`'s inbound `mavlink_parse_char()`, only ever sets `MAVLINK_STATUS_FLAG_IN_MAVLINK1`
(value `1`, a different bit) when it sees a MAVLink 1 STX byte; the two flags are read and
written independently, so nothing a ground station sends can downgrade this firmware's
output. Both ports therefore emit MAVLink 2 unconditionally, today, regardless of what
connects to them — a fact about the current code, not a default that could silently change
underneath this claim without `pio check` or a build failing to notice (it would not; this
is why task 3.1 asserts it against a live capture too, not just against the source).
*Alternative considered:* ship `0`, as an earlier draft of this design did, on the grounds
that no test captured the wire bytes. Superseded — the question turned out to be answerable
from the source alone, with no ambiguity left to resolve by capture, so leaving it
unclaimed would report `0` for something demonstrably `1`.

**3. `AUTOPILOT_VERSION` is enqueued before its `COMMAND_ACK`, both on the same
`xQueueSend` non-blocking, drop-on-full path every other reply already uses.**
There is no ordering requirement either message depends on; this project has no precedent
either way (the closest, `sendBootStatusText()` + `sendClockStatusText()`, posts two
`STATUSTEXT`s back to back with no ordering constraint between them). Sending the
substantive answer first means a GCS that does not wait for the ACK still has the data it
asked for.
*Alternative considered:* ACK first, mirroring `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`'s
ack-before-acting order. Rejected as not applicable — that ordering exists because the
action after it (a reset) is irreversible and needs the ACK to have a chance to reach the
link first; nothing here is irreversible.

**4. No queue depth change.** Two enqueues from one inbound command, on top of whatever
periodic entries are due in the same `TaskMavlink` pass, is a new burst this queue has not
been sized against by name — but the arithmetic already holds. `TaskMavlink` processes at
most one inbound message per loop iteration (it blocks on `xQueueReceive` for
`linkReadQueue`), so the worst single iteration enqueues the four periodic entries (if all
are simultaneously due) plus these two — six items, exactly the existing depth. No overflow,
no change needed. This is read from the schedule's own shape (`src/mavlink.cpp`), not
assumed; task 6 below is verifying it holds on the board rather than only on paper, because
a dropped `COMMAND_ACK` is silent and nothing currently counts it (`TODO.md`'s *"Emit
`SYS_STATUS`"* names `errors_count1..4` as where a dropped send would eventually be
counted, which this change does not implement).

## Risks / Trade-offs

- **[Risk]** A drop on the write queue is silent — as it already is for every existing
  `send*` call in this file. If the worst-case six-item pass above is wrong in practice
  (a periodic entry this design missed, or a second inbound command arriving before
  `TaskLinkWrite` drains the first reply), the ground sees `AUTOPILOT_VERSION`, its
  `COMMAND_ACK`, or both, silently dropped. → **Mitigation:** none new — this is the
  existing fire-and-forget behaviour of every outbound message in this firmware, not a
  regression this change introduces. Task 6 verifies the six-item bound on the board rather
  than trusting the count on paper.
- **[Risk]** `capabilities` carrying only `MAV_PROTOCOL_CAPABILITY_MAVLINK2` may read, to an
  unfamiliar GCS, as "this vehicle answers almost nothing," when the truer statement is
  "this vehicle answers only what the identity triple, `HEARTBEAT` and the wire protocol
  version already imply." → **Mitigation:** none needed — this is what `proposal.md`'s *Why*
  argues is the honest answer today, and it self-corrects as other `TODO.md` entries
  (parameters, FTP) land and set their own bit in the same constant.

## Migration Plan

No migration: this is a new reply to a previously-ignored command, with no stored state and
no change to any existing message. Nothing to roll back beyond reverting the commit.

## Open Questions

None. The one candidate question, whether `MAVLINK2` belongs in `capabilities`, turned out
to be answerable from the vendored library's source rather than needing a live capture —
see Decision 2.
