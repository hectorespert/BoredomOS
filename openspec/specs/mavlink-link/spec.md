# mavlink-link Specification

## Purpose
Defines which physical ports carry MAVLink to the ground station, at what speed,
how that choice is made at build time, what the firmware may assume about a port
being ready, and what independence two ports owe each other. There are two: the
hardware UART on D0/D1 and the USB CDC port. The `console-cli` capability, which
used to define the USB port's role as a text console, was retired by
`replace-console-cli-with-usb-mavlink-link` when that port became a link.

## Requirements

### Requirement: No task waits for the link port to become ready

No task SHALL block, spin or delay waiting for a link port to report itself ready
before producing or transmitting telemetry. The satellite SHALL operate with no host
and no ground station attached to either port.

A USB CDC port, unlike a hardware UART, has a real readiness state: with no host
attached, frames written to it cannot leave the board. The firmware SHALL discard
those frames rather than wait for a host. A host that is attached but has stopped
draining the port SHALL NOT stall any other task, SHALL NOT prevent the watchdog
from being refreshed, and SHALL NOT delay traffic on the other port; frames that
cannot be accepted SHALL be discarded, as they already are when an outbound queue
is full.

In the normal configuration, all tasks SHALL reach their steady-state cadence and
housekeeping records SHALL continue to be written to the SD card at 1 Hz. Whether
that holds in the reduced configuration, or with no SD card present, is governed by
the `fault-recovery` capability, not by this requirement.

#### Scenario: Board powered with nothing attached

- **WHEN** the board is powered with neither a host on USB nor a ground station on
  the UART, and starts in the normal configuration
- **THEN** all tasks reach their steady-state cadence
- **AND** housekeeping records continue to be written to the SD card at 1 Hz

#### Scenario: Ground station attached after boot

- **WHEN** a ground station connects to either port some time after boot
- **THEN** it begins receiving telemetry at the normal rates without the board being
  reset

#### Scenario: A host opens the USB port and stops reading

- **WHEN** a host opens the USB CDC port and then stops draining it, while the
  firmware keeps producing telemetry for that port
- **THEN** the board does not reset
- **AND** a ground station on the UART continues to receive telemetry at the normal
  rates
- **AND** housekeeping records continue to be written to the SD card at 1 Hz

### Requirement: Link traffic does not disturb periodic cadences

Transmitting MAVLink frames on either port SHALL NOT shift the firmware's periodic
cadences: housekeeping sampling stays at 1 Hz and telemetry keeps its declared rates
regardless of link traffic on either port.

This is a cadence guarantee, not a latency one. Transmission occupies the CPU for
the duration of a frame — a few milliseconds at link speed — and lower-priority
tasks do not run while it does. What SHALL hold is that this delay is absorbed
within each task's period rather than accumulating into drift.

#### Scenario: Sustained telemetry on a slow link

- **WHEN** the UART is transmitting continuously at the link baud rate
- **THEN** housekeeping records are still written once per second
- **AND** `HEARTBEAT` and `SYSTEM_TIME` still leave at 1 Hz on both ports

#### Scenario: Sustained inbound traffic on one port

- **WHEN** a ground station sends a continuous stream of MAVLink frames to one port
- **THEN** housekeeping records are still written once per second
- **AND** the other port keeps emitting `HEARTBEAT` and `SYSTEM_TIME` at 1 Hz

### Requirement: Housekeeping telemetry is available on request, never unsolicited

Each port SHALL be able to publish free heap, minimum-ever-free heap (in bytes), and
the stack high-water mark (in words) of every task that exists in the running
configuration, as `NAMED_VALUE_INT` messages, one message per schedule pass at a
ground-controlled interval, cycling through the full set before repeating. A task
that does not exist in the running configuration SHALL be omitted from the set rather
than reported as a value belonging to a different task.

This publishing SHALL be off by default on every port and SHALL start, on the port
the request arrived on, only after a ground station sends
`MAV_CMD_SET_MESSAGE_INTERVAL` targeting message id 252 (`NAMED_VALUE_INT`) with an
interval greater than or equal to 1000 ms, and SHALL stop when a ground station sends
that command on that port with an interval of `-1`. A request for "the default rate"
(an interval of `0`) SHALL NOT start publishing, and SHALL stop it if it was already
running, since the default rate is off in both cases. A request for a positive
interval below 1000 ms SHALL NOT start or change publishing, and SHALL be answered
with `COMMAND_ACK` / `MAV_RESULT_DENIED` rather than accepted or silently adjusted.
Every `MAV_CMD_SET_MESSAGE_INTERVAL` targeting message id 252 SHALL be answered with
`COMMAND_ACK`.

Arming SHALL be per port: a request on one port SHALL NOT start, stop or change
publishing on any other port, and each port SHALL keep its own position in the cycle.

This request SHALL NOT be persisted: after any reset, for any reason, publishing SHALL
be off on every port until a ground station requests it again in that session. Whether
the request reaches the firmware and is acted on SHALL NOT depend on whether the
firmware is in the normal or the reduced configuration; the set of values published,
however, follows which tasks exist in that configuration.

#### Scenario: No ground station has asked

- **WHEN** the firmware is running and no `MAV_CMD_SET_MESSAGE_INTERVAL` targeting
  message id 252 has been received on a port since the last reset
- **THEN** no `NAMED_VALUE_INT` message is sent on that port

#### Scenario: A ground station requests housekeeping

- **WHEN** a ground station sends `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id
  parameter set to 252 and the interval parameter at or above 1000 ms
- **THEN** the firmware replies with `COMMAND_ACK` / `MAV_RESULT_ACCEPTED`
- **AND** begins sending one `NAMED_VALUE_INT` message per schedule pass at that
  interval on that port, cycling through free heap, minimum-ever-free heap, then the
  stack high-water mark of every task that exists in the running configuration,
  before repeating

#### Scenario: Arming one port leaves the other alone

- **WHEN** a ground station arms housekeeping on one port and a ground station on the
  other port has not
- **THEN** `NAMED_VALUE_INT` messages are sent on the armed port only
- **AND** the unarmed port continues to carry its other telemetry unchanged

#### Scenario: A ground station requests an interval below the floor

- **WHEN** a ground station sends `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id
  parameter set to 252 and the interval parameter greater than zero but below 1000 ms
- **THEN** the firmware replies with `COMMAND_ACK` / `MAV_RESULT_DENIED`
- **AND** publishing state on every port (on, off, and the interval in effect if
  already on) is unchanged by the request

#### Scenario: A ground station disables housekeeping

- **WHEN** a ground station sends `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id
  parameter set to 252 and the interval parameter set to `-1`
- **THEN** the firmware replies with `COMMAND_ACK`
- **AND** stops sending `NAMED_VALUE_INT` messages on that port

#### Scenario: A ground station asks for the default rate

- **WHEN** a ground station sends `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id
  parameter set to 252 and the interval parameter set to `0`
- **THEN** the firmware replies with `COMMAND_ACK`
- **AND** `NAMED_VALUE_INT` messages are not sent on that port, whether or not
  publishing was already active there before this request

#### Scenario: The board resets after housekeeping was requested

- **WHEN** a ground station had requested housekeeping on either port and the
  firmware then resets, for any reason
- **THEN** after the reset, no `NAMED_VALUE_INT` message is sent on any port until a
  ground station requests it again

#### Scenario: Requested while in the reduced configuration

- **WHEN** the firmware is in the reduced configuration and a ground station sends
  `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id parameter set to 252 and the
  interval parameter at or above 1000 ms
- **THEN** the firmware replies with `COMMAND_ACK` / `MAV_RESULT_ACCEPTED` and begins
  sending `NAMED_VALUE_INT` messages on that port, cycling through free heap,
  minimum-ever-free heap, and the stack high-water mark of every task that exists in
  the reduced configuration — a smaller set than the normal configuration's, since
  not every task is created there

### Requirement: MAVLink is carried on both the UART and the USB CDC port

The firmware SHALL exchange MAVLink frames over the hardware UART exposed on pins
D0 (`RX`) and D1 (`TX`) at the link baud rate, and over the USB CDC port, in every
build. Neither port SHALL be a precondition for the other: each SHALL reach and hold
its steady-state cadence whether or not anything is attached to the other.

An inbound message that offers a value the firmware validates SHALL be acted upon when
the value passes validation and ignored when it does not, on either port. Ignoring one
SHALL NOT emit a message of its own, so that a peer cannot make the firmware generate
traffic by repeating a value the firmware will not accept.

#### Scenario: Ground station attached to the UART pins

- **WHEN** a ground station is connected to D0/D1 at the link baud rate
- **THEN** it receives `HEARTBEAT` and `SYSTEM_TIME` at 1 Hz and `BATTERY_STATUS`
  every 2 s
- **AND** inbound `SYSTEM_TIME` carrying a plausible time, and inbound `TIMESYNC`, sent
  on that port are acted upon

#### Scenario: Ground station attached to USB

- **WHEN** a host opens the USB CDC port and speaks MAVLink
- **THEN** it receives `HEARTBEAT` and `SYSTEM_TIME` at 1 Hz and `BATTERY_STATUS`
  every 2 s
- **AND** inbound `SYSTEM_TIME` carrying a plausible time, and inbound `TIMESYNC`, sent
  on that port are acted upon
- **AND** it receives no text: the port carries MAVLink frames only

#### Scenario: Only one port is attached

- **WHEN** a ground station is attached to one port and nothing to the other
- **THEN** the attached port carries the full telemetry set at its normal rates
- **AND** commands sent on it are acted upon

#### Scenario: A peer repeats a value the firmware will not accept

- **WHEN** a ground station sends `SYSTEM_TIME` carrying an implausible time
  repeatedly, as fast as the link allows
- **THEN** the periodic telemetry on that port keeps its normal rates
- **AND** no message is emitted in response to the rejected values

### Requirement: Each port is an independent MAVLink stream

Each port SHALL number its outbound frames with its own sequence counter,
incrementing by one per frame sent on that port and unaffected by traffic on the
other. A message sent in response to an inbound message SHALL leave by the port the
request arrived on, and SHALL NOT be emitted on any other port.

#### Scenario: Two ground stations attached at once

- **WHEN** a ground station is attached to each port and both observe the sequence
  field of the frames they receive
- **THEN** each sees a sequence that advances by one per frame, with no gaps
  attributable to the other port's traffic

#### Scenario: A command arrives on one port

- **WHEN** a ground station on one port sends a message the firmware answers —
  `TIMESYNC` with `tc1 == 0`, or `MAV_CMD_SET_MESSAGE_INTERVAL` targeting message
  id 252
- **THEN** the answer is received on that port
- **AND** a ground station on the other port does not receive it

### Requirement: `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` is answered

A `COMMAND_LONG` carrying `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` SHALL be answered, on
the port it arrived on, with an `AUTOPILOT_VERSION` message followed by `COMMAND_ACK` /
`MAV_RESULT_ACCEPTED` for that command. The reply SHALL leave by the port the request
arrived on and SHALL NOT be emitted on any other port, per the existing per-port reply
requirement.

`AUTOPILOT_VERSION.capabilities` SHALL report only protocol capabilities the firmware
actually implements or unconditionally provides. `MAV_PROTOCOL_CAPABILITY_MAVLINK2`
SHALL be set, since the firmware transmits MAVLink 2 on both ports unconditionally. A bit
SHALL NOT be set for a protocol this firmware does not answer — the mission protocol, the
parameter protocol and the File Transfer Protocol are not implemented today, so their
corresponding bits SHALL be clear. Every other field of `AUTOPILOT_VERSION` that this
firmware has no tracked value for (firmware, middleware and OS version, board version,
vendor id, product id, and the unique-id fields) SHALL be reported as `0` rather than a
fabricated value.

#### Scenario: A ground station requests capabilities

- **WHEN** a ground station sends `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` on either port
- **THEN** the firmware replies on that same port with `AUTOPILOT_VERSION`
- **AND** `capabilities` carries `MAV_PROTOCOL_CAPABILITY_MAVLINK2` and no bit for a
  protocol the firmware does not implement
- **AND** a `COMMAND_ACK` for `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` with
  `MAV_RESULT_ACCEPTED` follows on the same port

#### Scenario: Requested on one port does not answer on the other

- **WHEN** a ground station attached to one port sends
  `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES`
- **THEN** `AUTOPILOT_VERSION` and its `COMMAND_ACK` are emitted only on the port the
  request arrived on
- **AND** nothing is emitted on the other port

### Requirement: Every COMMAND_LONG receives a COMMAND_ACK

Every `COMMAND_LONG` the firmware receives, on either port, SHALL be answered
with a `COMMAND_ACK` for that command, on the port it arrived on. A command
this firmware does not implement SHALL NOT be left without a reply.

A `COMMAND_LONG` naming a `command` this firmware does not implement SHALL be
answered `COMMAND_ACK` / `MAV_RESULT_UNSUPPORTED`. This includes
`MAV_CMD_GET_HOME_POSITION`, for which the firmware holds no home position.

A `MAV_CMD_SET_MESSAGE_INTERVAL` naming a message id other than
`NAMED_VALUE_INT` (252) SHALL be answered `COMMAND_ACK` / `MAV_RESULT_DENIED`:
the command is recognised and valid, but this firmware does not let the ground
adjust that message's rate.

#### Scenario: An unimplemented command is rejected, not ignored

- **WHEN** a ground station sends a `COMMAND_LONG` naming a `command` this
  firmware does not implement
- **THEN** the firmware answers, on the port the request arrived on, with
  `COMMAND_ACK` for that command and `result` `MAV_RESULT_UNSUPPORTED`

#### Scenario: MAV_CMD_GET_HOME_POSITION is answered, not silently dropped

- **WHEN** a ground station sends `MAV_CMD_GET_HOME_POSITION`
- **THEN** the firmware answers, on the port the request arrived on, with
  `COMMAND_ACK` / `MAV_RESULT_UNSUPPORTED`

#### Scenario: An unsupported message-interval target is denied, not ignored

- **WHEN** a ground station sends `MAV_CMD_SET_MESSAGE_INTERVAL` naming a
  message id other than `NAMED_VALUE_INT` (252)
- **THEN** the firmware answers, on the port the request arrived on, with
  `COMMAND_ACK` / `MAV_RESULT_DENIED`, and the housekeeping stream's armed
  state on that port is left unchanged

### Requirement: Every port presents the same vehicle identity

Every outbound message SHALL carry system id `1`, component
`MAV_COMP_ID_AUTOPILOT1`, type `MAV_TYPE_ROCKET` and autopilot
`MAV_AUTOPILOT_GENERIC`, on every port. The satellite SHALL appear as one vehicle,
not one per port.

#### Scenario: Identity observed on either port

- **WHEN** a ground station is attached to the UART link, or to the USB port, or one
  to each
- **THEN** each sees exactly one vehicle, system `1`, component
  `MAV_COMP_ID_AUTOPILOT1`, type `MAV_TYPE_ROCKET`
- **AND** the two observers see the same vehicle, not two

### Requirement: Each MAVLink port is named by a single definition

Each port that carries MAVLink SHALL be named by a single definition that the whole
firmware refers to, and the UART's baud rate likewise. Selecting a different UART
port or speed SHALL be possible by overriding those definitions at build time,
without editing any file that implements the MAVLink protocol or a task body. A USB
CDC port has no line rate, so no baud definition SHALL apply to it.

#### Scenario: Changing the link speed

- **WHEN** the firmware is built with the UART baud rate overridden
- **THEN** the UART link operates at that speed and no other behaviour changes
- **AND** the USB port is unaffected

#### Scenario: Moving the UART link to a different hardware port

- **WHEN** the firmware is built with the UART port definition overridden to another
  hardware serial port
- **THEN** the build succeeds with no other source change
- **AND** a ground station on that port sees the same messages, at the same rates,
  with the same identity
