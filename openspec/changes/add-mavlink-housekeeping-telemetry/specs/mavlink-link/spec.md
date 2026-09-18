## ADDED Requirements

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
