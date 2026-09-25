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
- **THEN** it receives `HEARTBEAT`, `SYSTEM_TIME` and `SYS_STATUS` at 1 Hz and
  `BATTERY_STATUS` every 2 s
- **AND** inbound `SYSTEM_TIME` carrying a plausible time, and inbound `TIMESYNC`, sent
  on that port are acted upon

#### Scenario: Ground station attached to USB

- **WHEN** a host opens the USB CDC port and speaks MAVLink
- **THEN** it receives `HEARTBEAT`, `SYSTEM_TIME` and `SYS_STATUS` at 1 Hz and
  `BATTERY_STATUS` every 2 s
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

### Requirement: SYS_STATUS reports sensor presence, battery state and link error counters every second

Each port SHALL emit `SYS_STATUS` at 1000 ms, unconditionally — including in the
reduced configuration — alongside its `HEARTBEAT` and `SYSTEM_TIME` cadence. The
rate SHALL NOT be adjustable by `MAV_CMD_SET_MESSAGE_INTERVAL`, per the existing
requirement that a message id other than `NAMED_VALUE_INT` (252) is denied.

`onboard_control_sensors_present`, `_enabled` and `_health` SHALL carry the same
value for each bit this firmware reports: the SD card's presence in
`MAV_SYS_STATUS_LOGGING`, and the battery sense's presence in
`MAV_SYS_STATUS_SENSOR_BATTERY`, except as the reduced configuration modifies it
below. The real-time clock's presence SHALL NOT be represented in these
bitmaps; its absence remains reported only through `STATUSTEXT`.

`voltage_battery` and `battery_remaining` SHALL report the same reading
`BATTERY_STATUS` reports at that moment, rather than the protocol's "not sent"
sentinel. `current_battery` SHALL report the protocol's "not sent" sentinel
(`-1`): this firmware has no current sensor, only the voltage ADC.

The reduced configuration SHALL NOT depend on the battery sense, per the
`fault-recovery` capability. In that configuration, `MAV_SYS_STATUS_SENSOR_BATTERY`
SHALL be clear in all three bitmaps, and `voltage_battery` and `battery_remaining`
SHALL report the protocol's "not sent" sentinel rather than a reading — `SYS_STATUS`
itself is not withheld the way `BATTERY_STATUS` is, only its battery fields.

`errors_count1` SHALL count, per port and saturating rather than wrapping at its
maximum, every MAVLink frame this firmware could not queue for transmission on
that port because that port's outbound queue was full. `errors_comm` SHALL
count, per port, MAVLink frames received on that port that failed to parse.
`load`, `drop_rate_comm` and `errors_count2..4` SHALL be reported as `0`.

#### Scenario: SYS_STATUS is sent every second on each port

- **WHEN** the firmware is running, in either configuration
- **THEN** a ground station connected to either port receives a `SYS_STATUS`
  message approximately once per second on that port

#### Scenario: The SD card's presence is reflected in the sensor bitmap

- **WHEN** the board boots with an SD card present
- **THEN** `MAV_SYS_STATUS_LOGGING` is set in `onboard_control_sensors_present`,
  `_enabled` and `_health`

#### Scenario: The SD card's absence is reflected in the sensor bitmap

- **WHEN** the board boots with no SD card present
- **THEN** `MAV_SYS_STATUS_LOGGING` is clear in `onboard_control_sensors_present`,
  `_enabled` and `_health`

#### Scenario: Battery fields match BATTERY_STATUS

- **WHEN** a ground station reads `SYS_STATUS` and `BATTERY_STATUS` from the same
  port
- **THEN** `voltage_battery` and `battery_remaining` report the same battery
  state `BATTERY_STATUS` reports

#### Scenario: The reduced configuration does not report the battery sense

- **WHEN** the firmware is in the reduced configuration
- **THEN** `MAV_SYS_STATUS_SENSOR_BATTERY` is clear in `onboard_control_sensors_present`,
  `_enabled` and `_health`
- **AND** `voltage_battery` and `battery_remaining` report the protocol's "not
  sent" sentinel rather than a reading

#### Scenario: A dropped outbound frame is counted

- **WHEN** a port's outbound queue is full and a frame for that port is dropped
  as a result
- **THEN** that port's next `SYS_STATUS` reports a higher `errors_count1` than
  its previous one

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

### Requirement: `MAV_CMD_REQUEST_MESSAGE` sends the requested message once

A `COMMAND_LONG` carrying `MAV_CMD_REQUEST_MESSAGE`, whose first parameter names one of
`HEARTBEAT` (0), `SYS_STATUS` (1), `SYSTEM_TIME` (2), `BATTERY_STATUS` (147),
`AUTOPILOT_VERSION` (148) or `PROTOCOL_VERSION` (300), SHALL be answered with one instance
of that message and with `COMMAND_ACK` / `MAV_RESULT_ACCEPTED` for the command, both on the
port the request arrived on. The message SHALL carry the content the periodic or constant
form of it carries at that moment. The message SHALL be emitted on the requesting port only,
whatever the request's target-address parameter says. A request SHALL NOT start, stop or
retime any periodic stream, on any port.

A request naming any other message id, including `NAMED_VALUE_INT` (252), whose housekeeping
values are published one per pass and are not a single message that can be sent once,
SHALL be answered `COMMAND_ACK` / `MAV_RESULT_DENIED` and SHALL NOT cause any message to be
emitted: the command is recognised and valid, but this firmware does not serve that message
on request.

A request for `BATTERY_STATUS` in the reduced configuration SHALL be answered
`COMMAND_ACK` / `MAV_RESULT_TEMPORARILY_REJECTED` and SHALL NOT cause a `BATTERY_STATUS` to
be emitted, since the reduced configuration does not read the battery
(`fault-recovery`) and a message of zeros would be a false reading.

#### Scenario: A served message is requested

- **WHEN** a ground station sends `MAV_CMD_REQUEST_MESSAGE` with the message id parameter
  set to 2 (`SYSTEM_TIME`)
- **THEN** it receives one `SYSTEM_TIME` message on that port, in addition to the
  `SYSTEM_TIME` the periodic schedule sends
- **AND** it receives `COMMAND_ACK` for `MAV_CMD_REQUEST_MESSAGE` with
  `MAV_RESULT_ACCEPTED` on that port

#### Scenario: Every served id is answered with its own message

- **WHEN** a ground station sends `MAV_CMD_REQUEST_MESSAGE` once for each of the ids 0, 1,
  2, 147, 148 and 300, in the normal configuration
- **THEN** each request is answered with one message of the id requested and
  `MAV_RESULT_ACCEPTED`

#### Scenario: A request does not disturb the periodic cadences

- **WHEN** a ground station sends `MAV_CMD_REQUEST_MESSAGE` for `HEARTBEAT` repeatedly
- **THEN** the periodic `HEARTBEAT`, `SYSTEM_TIME` and `SYS_STATUS` on both ports keep
  their 1 Hz cadence, and `BATTERY_STATUS` its 2 s cadence

#### Scenario: A message this firmware does not serve on request

- **WHEN** a ground station sends `MAV_CMD_REQUEST_MESSAGE` with a message id outside the
  served set, or with 252
- **THEN** the firmware answers `COMMAND_ACK` / `MAV_RESULT_DENIED` on that port
- **AND** no message of the requested id is emitted as a result

#### Scenario: BATTERY_STATUS requested with no battery reading

- **WHEN** the firmware is in the reduced configuration and a ground station sends
  `MAV_CMD_REQUEST_MESSAGE` for `BATTERY_STATUS`
- **THEN** the firmware answers `COMMAND_ACK` / `MAV_RESULT_TEMPORARILY_REJECTED`
- **AND** no `BATTERY_STATUS` is emitted on either port

#### Scenario: Requested on one port does not answer on the other

- **WHEN** a ground station on one port sends `MAV_CMD_REQUEST_MESSAGE` for a served id
  with the target-address parameter set to broadcast
- **THEN** the message and its `COMMAND_ACK` are received on that port only

### Requirement: `MAV_CMD_GET_MESSAGE_INTERVAL` reports the interval a message really has

A `COMMAND_LONG` carrying `MAV_CMD_GET_MESSAGE_INTERVAL` SHALL be answered, on the port it
arrived on, with `COMMAND_ACK` / `MAV_RESULT_ACCEPTED` followed by a `MESSAGE_INTERVAL` whose
`message_id` is the id requested and whose `interval_us` is what that message's stream is
on **that port** at that moment:

- for a message the firmware sends periodically, its period in microseconds — 1000000 for
  `HEARTBEAT` (0), `SYS_STATUS` (1) and `SYSTEM_TIME` (2), and 2000000 for `BATTERY_STATUS`
  (147);
- `-1` for a message whose stream is off on that port: `BATTERY_STATUS` in the reduced
  configuration, where it is withheld; `NAMED_VALUE_INT` (252) until a ground station arms
  it there; and a message the firmware sends only on request, `AUTOPILOT_VERSION` (148) and
  `PROTOCOL_VERSION` (300);
- the interval that was armed, in microseconds, for `NAMED_VALUE_INT` (252) once armed on
  that port;
- `0` (not available) for any other id: one the ground can obtain neither by a stream
  nor by `MAV_CMD_REQUEST_MESSAGE`.

The reported interval SHALL be the one the firmware is using, so that the value observed
on the wire and the value reported cannot disagree. The answer for one port SHALL NOT
depend on how another port was configured.

A message id parameter that is not a valid message id — not a number, not a whole number, or
outside 0 to 65535 — SHALL be answered `COMMAND_ACK` / `MAV_RESULT_DENIED` with no `MESSAGE_INTERVAL`,
and SHALL NOT be read as the id its low 16 bits would spell. The same holds for
`MAV_CMD_REQUEST_MESSAGE`, whose refusal of an id outside the served set already covers it.

#### Scenario: A periodic message's interval is reported

- **WHEN** a ground station sends `MAV_CMD_GET_MESSAGE_INTERVAL` for id 0 on a port
- **THEN** it receives `COMMAND_ACK` / `MAV_RESULT_ACCEPTED`, then `MESSAGE_INTERVAL`
  with `message_id` 0 and `interval_us` 1000000, on that port

#### Scenario: The housekeeping stream reports its armed state per port

- **WHEN** a ground station has armed housekeeping on one port with
  `MAV_CMD_SET_MESSAGE_INTERVAL` at 2000000 µs, and then sends
  `MAV_CMD_GET_MESSAGE_INTERVAL` for id 252 on that port and on the other
- **THEN** the port it armed reports `interval_us` 2000000
- **AND** the other port reports `interval_us` -1

#### Scenario: A stream that has been switched off reports so

- **WHEN** a ground station has armed housekeeping and then disabled it with an interval
  of `-1`, and sends `MAV_CMD_GET_MESSAGE_INTERVAL` for id 252
- **THEN** it receives `MESSAGE_INTERVAL` with `interval_us` -1

#### Scenario: A message sent only on request reports no stream

- **WHEN** a ground station sends `MAV_CMD_GET_MESSAGE_INTERVAL` for id 148
- **THEN** it receives `MESSAGE_INTERVAL` with `message_id` 148 and `interval_us` -1

#### Scenario: BATTERY_STATUS in the reduced configuration reports no stream

- **WHEN** the firmware is in the reduced configuration and a ground station sends
  `MAV_CMD_GET_MESSAGE_INTERVAL` for id 147
- **THEN** it receives `MESSAGE_INTERVAL` with `interval_us` -1

#### Scenario: A message this firmware cannot provide

- **WHEN** a ground station sends `MAV_CMD_GET_MESSAGE_INTERVAL` for an id this firmware
  neither streams nor serves on request
- **THEN** it receives `COMMAND_ACK` / `MAV_RESULT_ACCEPTED` and `MESSAGE_INTERVAL` with
  that `message_id` and `interval_us` 0

#### Scenario: A message id that is not a valid id

- **WHEN** a ground station sends `MAV_CMD_GET_MESSAGE_INTERVAL` with the message id
  parameter set to 65538 (`0x10002`, whose low 16 bits are the id of `SYSTEM_TIME`)
- **THEN** the firmware answers `COMMAND_ACK` / `MAV_RESULT_DENIED`
- **AND** no `MESSAGE_INTERVAL` is emitted, and `MAV_CMD_REQUEST_MESSAGE` with the same
  parameter causes no `SYSTEM_TIME` to be emitted

### Requirement: `PROTOCOL_VERSION` is sent on request and reports MAVLink 2 only

`PROTOCOL_VERSION` SHALL be sent only in answer to `MAV_CMD_REQUEST_MESSAGE` naming id 300,
never unsolicited. It SHALL report `version`, `min_version` and `max_version` as 200: the
firmware transmits MAVLink 2 unconditionally on both ports, so it SHALL NOT claim a
minimum a MAVLink 1 peer could not follow. `spec_version_hash` and `library_version_hash`
SHALL be all zero: the firmware has no tracked value for either, and a fabricated hash is
worse than none, as for the unset fields of `AUTOPILOT_VERSION`.

#### Scenario: PROTOCOL_VERSION requested

- **WHEN** a ground station sends `MAV_CMD_REQUEST_MESSAGE` for id 300
- **THEN** it receives one `PROTOCOL_VERSION` with `version`, `min_version` and
  `max_version` equal to 200 and both hashes zero, and `COMMAND_ACK` /
  `MAV_RESULT_ACCEPTED`, on that port

#### Scenario: PROTOCOL_VERSION is not sent unasked

- **WHEN** a ground station listens on a port for a minute without sending anything
- **THEN** it receives no `PROTOCOL_VERSION`

### Requirement: A mission list request is answered with an empty mission

A `MISSION_REQUEST_LIST` SHALL be answered, on the port it arrived on, with
`MISSION_COUNT` whose `count` is `0`, whose `mission_type` is the one the request named, and
whose target system and component are those of the sender of the request. The firmware holds
no mission of any type, and SHALL NOT answer this request with a `STATUSTEXT` about an
unhandled message.

Answering the list request SHALL NOT be taken for implementing the mission protocol: no
message that transfers or edits a mission item is handled, and `AUTOPILOT_VERSION.capabilities`
SHALL keep every mission capability bit clear.

#### Scenario: The ground asks for the mission

- **WHEN** a ground station sends `MISSION_REQUEST_LIST` with `mission_type`
  `MAV_MISSION_TYPE_MISSION`
- **THEN** it receives `MISSION_COUNT` with `count` 0 and `mission_type`
  `MAV_MISSION_TYPE_MISSION`, on that port, addressed to the sender
- **AND** it receives no `STATUSTEXT` for that request

#### Scenario: The mission type is echoed

- **WHEN** a ground station sends `MISSION_REQUEST_LIST` with `mission_type`
  `MAV_MISSION_TYPE_FENCE`
- **THEN** the `MISSION_COUNT` it receives has `count` 0 and `mission_type`
  `MAV_MISSION_TYPE_FENCE`

#### Scenario: The mission protocol is not claimed

- **WHEN** a ground station requests `AUTOPILOT_VERSION`
- **THEN** `capabilities` has no mission protocol bit set
