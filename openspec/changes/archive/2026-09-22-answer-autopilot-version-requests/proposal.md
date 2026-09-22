## Why

`TaskMavlink`'s `COMMAND_LONG` switch has a case for `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES`
(520) that is deliberately empty, one of the reserved slots `ARCHITECTURE.md` §5.1 names.
This is the first thing a MAVLink ground station asks after connecting, to learn what the
vehicle can do; with no answer it treats the satellite as a minimal node and, depending on
the GCS, keeps retrying. `TODO.md`'s *"Answer the GCS messages that are ignored today"*
names this as the cheap, high-value one of the three it lists — cheaper than the parameter
protocol (which needs a storage decision before it answers anything useful) and standalone
from `REQUEST_DATA_STREAM` (already superseded by `MAV_CMD_SET_MESSAGE_INTERVAL`, which this
firmware answers today only for message id 252).

Answering honestly is also the point, not just answering. This firmware supports none of
the mission, parameter or FTP protocols yet, so the correct `AUTOPILOT_VERSION.capabilities`
bitmap is near-empty — and that emptiness is itself useful telemetry: it tells a GCS not to
keep probing for features that are not there, rather than the silence it gets today, which
looks identical to "the request did not arrive" and invites more of it.

## What Changes

- **`MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` gets a real case in `TaskMavlink`'s
  `COMMAND_LONG` switch.** It replies with `AUTOPILOT_VERSION` (148) on the port the
  request arrived on, followed by `COMMAND_ACK` / `MAV_RESULT_ACCEPTED` — the same
  "always ACK a `COMMAND_LONG`" policy this switch already holds for
  `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` and `MAV_CMD_SET_MESSAGE_INTERVAL`.
- **`AUTOPILOT_VERSION`'s fields are compile-time constants, not runtime state.**
  `capabilities` carries exactly one bit, `MAV_PROTOCOL_CAPABILITY_MAVLINK2` — verified
  true by reading the vendored MAVLink library rather than assumed (`design.md` Decision
  2): nothing in this project ever calls `mavlink_set_proto_version()`, so both ports emit
  MAVLink 2 unconditionally. No mission, parameter, FTP, or any other protocol bit is
  claimed, because none is implemented. `flight_sw_version`, `middleware_sw_version`,
  `os_sw_version`, `board_version`, `vendor_id`, `product_id`, `uid`, `uid2` and the
  three `*_custom_version` byte arrays are all `0` — nothing in this project tracks a
  build version, a board revision or a vendor/product id today, and inventing one to
  fill a field is worse than reporting it as absent.
- **No new `LinkMsg` payload.** Because every field is a constant, `mavlinkPack()`
  builds the whole message itself when it sees the new `LinkMsgKind::AutopilotVersion`
  tag — the same shape `LinkMsgKind::Heartbeat` already uses ("`kind` alone is the
  whole message"). The outbound queue item stays the 64-byte `LinkMsg` it is today;
  see `design.md`.
- **A new HIL check module**, `test/test_hil/check_capabilities.py`, sends
  `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` and asserts the reply: `AUTOPILOT_VERSION`
  arrives with `capabilities == MAV_PROTOCOL_CAPABILITY_MAVLINK2` and no other bit set,
  and a `COMMAND_ACK` for that command follows with `MAV_RESULT_ACCEPTED`.
- **`ARCHITECTURE.md` §5.1 updated in the same commit**: the sentence listing
  `COMMAND_LONG`'s cases as "deliberately empty" reserved slots no longer describes
  `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES`, which joins `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`
  and `MAV_CMD_SET_MESSAGE_INTERVAL` as one that is actually handled.

Out of scope, left for later entries already in `TODO.md`: `PARAM_REQUEST_LIST` (its own
entry, *"Implement the MAVLink parameter protocol"*, blocked on a storage decision) and
`REQUEST_DATA_STREAM` (superseded, not this change's concern).

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `mavlink-link`: gains a requirement that `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` is
  answered, on the port it arrived on, with `AUTOPILOT_VERSION` reporting only the
  `MAV_PROTOCOL_CAPABILITY_MAVLINK2` bit (the one true capability this firmware has)
  followed by `COMMAND_ACK` / `MAV_RESULT_ACCEPTED`. No change to the identity triple,
  the link ports, or any existing requirement's wording.

## Impact

- `src/mavlink.cpp`: new case in the `COMMAND_LONG` switch; new
  `LinkMsgKind::AutopilotVersion` case in `mavlinkPack()`.
- `include/LinkMsg.h`: one new `LinkMsgKind` enumerator. No change to the union, no
  change to `sizeof(LinkMsg)`, no change to the `static_assert` it carries.
- `test/test_hil/check_capabilities.py` (new).
- `ARCHITECTURE.md` §5.1.
- No queue depth change, no new task, no new library. RAM impact is bounded to a few
  bytes of stack inside `TaskLinkWrite`'s existing frame for the constant arrays
  `mavlinkPack()` passes to `mavlink_msg_autopilot_version_pack_chan()` — see
  `design.md` for the figure and the high-water-mark check this still owes.
