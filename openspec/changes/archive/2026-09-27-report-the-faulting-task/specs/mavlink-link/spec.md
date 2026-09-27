# Spec Delta

## MODIFIED Requirements

### Requirement: `MAV_CMD_REQUEST_MESSAGE` sends the requested message once

A `COMMAND_LONG` carrying `MAV_CMD_REQUEST_MESSAGE`, whose first parameter names one of
`HEARTBEAT` (0), `SYS_STATUS` (1), `SYSTEM_TIME` (2), `BATTERY_STATUS` (147),
`AUTOPILOT_VERSION` (148), `STATUSTEXT` (253) or `PROTOCOL_VERSION` (300), SHALL be answered
with one instance of that message and with `COMMAND_ACK` / `MAV_RESULT_ACCEPTED` for the
command, both on the port the request arrived on. The message SHALL carry the content the
periodic or constant form of it carries at that moment. The message SHALL be emitted on the
requesting port only, whatever the request's target-address parameter says. A request SHALL
NOT start, stop or retime any periodic stream, on any port.

`STATUSTEXT` has no periodic or constant form; the one sent on request SHALL be the
statement of the previous reset's cause that the firmware emits unasked at boot
(`fault-recovery`), with the same text and severity, whichever other `STATUSTEXT` the
firmware has sent since.

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
  2, 147, 148, 253 and 300, in the normal configuration
- **THEN** each request is answered with one message of the id requested and
  `MAV_RESULT_ACCEPTED`

#### Scenario: The boot statement is requested

- **WHEN** a ground station sends `MAV_CMD_REQUEST_MESSAGE` for 253, in either
  configuration
- **THEN** it receives `MAV_RESULT_ACCEPTED` and one `STATUSTEXT` on that port whose text
  begins with the reset statement's prefix and names the previous reset's cause as the
  heartbeat reports it

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
  it there; and a message the firmware sends only on request or on an event rather than on
  a schedule, `AUTOPILOT_VERSION` (148), `STATUSTEXT` (253) and `PROTOCOL_VERSION` (300);
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

- **WHEN** a ground station sends `MAV_CMD_GET_MESSAGE_INTERVAL` for id 148, and again for
  id 253
- **THEN** each is answered with `MESSAGE_INTERVAL` carrying the requested `message_id` and
  `interval_us` -1

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
