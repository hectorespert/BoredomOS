# mavlink-link Specification

## Purpose
Defines which physical port carries the MAVLink link to the ground station, at what
speed, how that choice is made at build time, and what the firmware may assume about
the port being ready. The USB port's role as a text console is defined by the
`console-cli` capability.

## Requirements

### Requirement: The MAVLink link is carried on the hardware UART

The firmware SHALL exchange MAVLink frames over the hardware UART exposed on pins D0
(`RX`) and D1 (`TX`), at the link baud rate, and SHALL NOT exchange MAVLink frames
over the USB CDC port.

#### Scenario: Ground station attached to the UART pins

- **WHEN** a ground station is connected to D0/D1 at the link baud rate
- **THEN** it receives `HEARTBEAT` and `SYSTEM_TIME` at 1 Hz and `BATTERY_STATUS`
  every 2 s
- **AND** inbound `SYSTEM_TIME` and `TIMESYNC` sent on that port are acted upon

#### Scenario: Host attached to USB

- **WHEN** a host opens the USB CDC port at 115200
- **THEN** it receives no MAVLink frames
- **AND** it receives no unsolicited text; the console answers text commands, and the
  reservation the console previously held is now taken by the `console-cli` capability,
  which defines what it emits and when

### Requirement: The link port and baud rate are defined in one place

The link port and its baud rate SHALL each be named by a single definition that the
whole firmware refers to. Selecting a different port or speed SHALL be possible by
overriding those definitions at build time, without editing any file that implements
the MAVLink protocol or a task body.

#### Scenario: Reverting the link to USB for bench work

- **WHEN** the firmware is built with the link port overridden to the USB CDC port
- **THEN** the build succeeds with no other source change
- **AND** a ground station on the USB port sees the same messages, at the same rates,
  with the same identity as it did before this change

#### Scenario: Changing the link speed

- **WHEN** the firmware is built with the link baud rate overridden
- **THEN** the link operates at that speed and no other behaviour changes

### Requirement: No task waits for the link port to become ready

No task SHALL block, spin or delay waiting for the link port to report itself ready
before producing or transmitting telemetry. The satellite SHALL operate with no host
and no ground station attached.

In the normal configuration, all tasks SHALL reach their steady-state cadence and
housekeeping records SHALL continue to be written to the SD card at 1 Hz. Whether that
holds in the reduced configuration, or with no SD card present, is governed by the
`fault-recovery` capability, not by this requirement.

#### Scenario: Board powered with nothing attached

- **WHEN** the board is powered with neither USB nor a ground station connected, and
  starts in the normal configuration
- **THEN** all tasks reach their steady-state cadence
- **AND** housekeeping records continue to be written to the SD card at 1 Hz

#### Scenario: Ground station attached after boot

- **WHEN** a ground station connects to the link some time after boot
- **THEN** it begins receiving telemetry at the normal rates without the board being
  reset

### Requirement: Link traffic does not disturb periodic cadences

Transmitting MAVLink frames SHALL NOT shift the firmware's periodic cadences:
housekeeping sampling stays at 1 Hz and telemetry keeps its declared rates
regardless of link traffic.

This is a cadence guarantee, not a latency one. Transmission occupies the CPU for
the duration of a frame — a few milliseconds at link speed — and lower-priority
tasks do not run while it does. What SHALL hold is that this delay is absorbed
within each task's period rather than accumulating into drift.

#### Scenario: Sustained telemetry on a slow link

- **WHEN** the link is transmitting continuously at the link baud rate
- **THEN** housekeeping records are still written once per second
- **AND** `HEARTBEAT` and `SYSTEM_TIME` still leave at 1 Hz

### Requirement: The vehicle identity is unchanged by the move

Every outbound message SHALL keep system id `1`, component `MAV_COMP_ID_AUTOPILOT1`,
type `MAV_TYPE_ROCKET` and autopilot `MAV_AUTOPILOT_GENERIC`, on whichever port the
link is carried.

#### Scenario: Identity observed on either port

- **WHEN** a ground station is connected to the UART link, or to USB in the
  overridden build
- **THEN** it sees exactly one vehicle, system `1`, component
  `MAV_COMP_ID_AUTOPILOT1`, type `MAV_TYPE_ROCKET`

### Requirement: Housekeeping telemetry is available on request, never unsolicited

The link SHALL be able to publish free heap, minimum-ever-free heap (in bytes), and
the stack high-water mark (in words) of every task that exists in the running
configuration, as `NAMED_VALUE_INT` messages, one message per schedule pass at a
ground-controlled interval, cycling through the full set before repeating. A task
that does not exist in the running configuration SHALL be omitted from the set rather
than reported as a value belonging to a different task.

This publishing SHALL be off by default and SHALL start only after a ground station
sends `MAV_CMD_SET_MESSAGE_INTERVAL` targeting message id 252 (`NAMED_VALUE_INT`)
with an interval greater than or equal to 1000 ms, and SHALL stop when a ground
station sends that command with an interval of `-1`. A request for "the default rate"
(an interval of `0`) SHALL NOT start publishing, and SHALL stop it if it was already
running, since the default rate is off in both cases. A request for a positive
interval below 1000 ms SHALL NOT start or change publishing, and SHALL be answered
with `COMMAND_ACK` / `MAV_RESULT_DENIED` rather than accepted or silently adjusted.
Every `MAV_CMD_SET_MESSAGE_INTERVAL` targeting message id 252 SHALL be answered with
`COMMAND_ACK`.

This request SHALL NOT be persisted: after any reset, for any reason, publishing SHALL
be off until a ground station requests it again in that session. Whether the request
reaches the firmware and is acted on SHALL NOT depend on whether the firmware is in the
normal or the reduced configuration; the set of values published, however, follows
which tasks exist in that configuration.

#### Scenario: No ground station has asked

- **WHEN** the firmware is running and no `MAV_CMD_SET_MESSAGE_INTERVAL` targeting
  message id 252 has been received since the last reset
- **THEN** no `NAMED_VALUE_INT` message is sent on the link

#### Scenario: A ground station requests housekeeping

- **WHEN** a ground station sends `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id
  parameter set to 252 and the interval parameter at or above 1000 ms
- **THEN** the firmware replies with `COMMAND_ACK` / `MAV_RESULT_ACCEPTED`
- **AND** begins sending one `NAMED_VALUE_INT` message per schedule pass at that
  interval, cycling through free heap, minimum-ever-free heap, then the stack
  high-water mark of every task that exists in the running configuration, before
  repeating

#### Scenario: A ground station requests an interval below the floor

- **WHEN** a ground station sends `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id
  parameter set to 252 and the interval parameter greater than zero but below 1000 ms
- **THEN** the firmware replies with `COMMAND_ACK` / `MAV_RESULT_DENIED`
- **AND** publishing state (on, off, and the interval in effect if already on) is
  unchanged by the request

#### Scenario: A ground station disables housekeeping

- **WHEN** a ground station sends `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id
  parameter set to 252 and the interval parameter set to `-1`
- **THEN** the firmware replies with `COMMAND_ACK`
- **AND** stops sending `NAMED_VALUE_INT` messages

#### Scenario: A ground station asks for the default rate

- **WHEN** a ground station sends `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id
  parameter set to 252 and the interval parameter set to `0`
- **THEN** the firmware replies with `COMMAND_ACK`
- **AND** `NAMED_VALUE_INT` messages are not sent, whether or not publishing was
  already active before this request

#### Scenario: The board resets after housekeeping was requested

- **WHEN** a ground station had requested housekeeping and the firmware then resets,
  for any reason
- **THEN** after the reset, no `NAMED_VALUE_INT` message is sent until a ground
  station requests it again

#### Scenario: Requested while in the reduced configuration

- **WHEN** the firmware is in the reduced configuration and a ground station sends
  `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id parameter set to 252 and the
  interval parameter at or above 1000 ms
- **THEN** the firmware replies with `COMMAND_ACK` / `MAV_RESULT_ACCEPTED` and begins
  sending `NAMED_VALUE_INT` messages, cycling through free heap, minimum-ever-free
  heap, and the stack high-water mark of every task that exists in the reduced
  configuration — a smaller set than the normal configuration's, since not every task
  is created there
