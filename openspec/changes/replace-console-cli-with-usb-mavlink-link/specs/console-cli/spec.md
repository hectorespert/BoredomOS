## REMOVED Requirements

Every requirement this capability states describes a text console on the USB CDC
port. That port becomes a MAVLink endpoint, so the console does not move, shrink
or change form — it ceases to exist, and the capability is removed rather than
modified. The reason and the migration are the same for all nine and are stated
once here rather than repeated verbatim under each.

**Reason**: The USB CDC port is now a second MAVLink endpoint
(`mavlink-link`). A text console and a MAVLink stream cannot share one port
without a framing discipline this change deliberately does not introduce, and
the port is more valuable as a link than as a console: it is what lets the HIL
suite reach the flight build with no adapter, and what lets a board latched into
the reduced configuration be commanded back out.

**Migration**: The observable values the console reported — free heap,
minimum-ever-free heap, and every live task's stack high-water mark — remain
available as the `NAMED_VALUE_INT` housekeeping stream defined by
`mavlink-link`, on either port, armed by
`MAV_CMD_SET_MESSAGE_INTERVAL`. What is not carried over is reaching them from a
plain terminal: a ground station speaking MAVLink is now required, the values
arrive one per schedule pass rather than all at once, and arming does not survive
a reset. There is no replacement for `help`/`?`, which described a command set
that no longer exists.

### Requirement: The console CLI is carried on the USB CDC port

**Reason**: See above — the USB CDC port now carries MAVLink.
**Migration**: See above.

### Requirement: The CLI is the console port's only writer while tasks run

**Reason**: See above. The port's single owner is now the link layer, and the
ownership rule itself survives under `mavlink-link` rather than here.
**Migration**: See above.

### Requirement: Commands are newline-terminated and answered before the next is read

**Reason**: See above — there are no text commands.
**Migration**: See above.

### Requirement: `ps` reports every task the kernel knows about

**Reason**: See above.
**Migration**: The per-task stack high-water marks are in the housekeeping
stream, with the caveats stated above.

### Requirement: `ps` accepts a task name to report a single task

**Reason**: See above.
**Migration**: No equivalent. The housekeeping stream cycles the whole set and
cannot be asked for one task.

### Requirement: `free` reports the heap

**Reason**: See above.
**Migration**: Free heap and minimum-ever-free heap are in the housekeeping
stream. The heap total is not published and has no equivalent.

### Requirement: `help` lists the available commands

**Reason**: See above.
**Migration**: No equivalent, and none is needed — it described a command set
that ceases to exist.

### Requirement: The CLI is read-only

**Reason**: See above. The guarantee that a diagnostic path cannot change
firmware state is not carried forward, because the replacement path is MAVLink,
which accepts commands that deliberately do change state.
**Migration**: None. This is a real reduction: the USB port goes from being
incapable of changing firmware state to accepting every inbound message the link
accepts, including `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`. That is the point of the
change, not an oversight.

### Requirement: The CLI never delays a flight task

**Reason**: See above.
**Migration**: The equivalent guarantee for the USB port is carried by
`mavlink-link`'s requirements that no task waits for a port to become ready and
that link traffic does not disturb periodic cadences, both of which this change
extends to cover a host that stops draining the CDC port.
