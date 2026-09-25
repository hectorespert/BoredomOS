## ADDED Requirements

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
