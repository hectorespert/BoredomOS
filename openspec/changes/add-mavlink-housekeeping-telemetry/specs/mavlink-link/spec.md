## ADDED Requirements

### Requirement: Housekeeping telemetry is available on request, never unsolicited

The link SHALL be able to publish free heap, minimum-ever-free heap, and each task's
stack high-water mark as `NAMED_VALUE_INT` messages, one message per schedule pass at
a ground-controlled interval, cycling through the full set before repeating. This
publishing SHALL be off by default and SHALL start only after a ground station sends
`MAV_CMD_SET_MESSAGE_INTERVAL` targeting message id 252 (`NAMED_VALUE_INT`) with an
interval greater than zero, and SHALL stop when a ground station sends that command
with an interval of `-1`. A request for "the default rate" (an interval of `0`) SHALL
NOT start publishing, since the default rate is off. Every `MAV_CMD_SET_MESSAGE_INTERVAL`
targeting message id 252 SHALL be answered with `COMMAND_ACK`.

This request SHALL NOT be persisted: after any reset, for any reason, publishing SHALL
be off until a ground station requests it again in that session. Whether the request
reaches the firmware and is acted on SHALL NOT depend on whether the firmware is in the
normal or the reduced configuration.

#### Scenario: No ground station has asked

- **WHEN** the firmware is running and no `MAV_CMD_SET_MESSAGE_INTERVAL` targeting
  message id 252 has been received since the last reset
- **THEN** no `NAMED_VALUE_INT` message is sent on the link

#### Scenario: A ground station requests housekeeping

- **WHEN** a ground station sends `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id
  parameter set to 252 and the interval parameter greater than zero
- **THEN** the firmware replies with `COMMAND_ACK`
- **AND** begins sending one `NAMED_VALUE_INT` message per schedule pass at that
  interval, cycling through free heap, minimum-ever-free heap, then each task's stack
  high-water mark, before repeating

#### Scenario: A ground station disables housekeeping

- **WHEN** a ground station sends `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id
  parameter set to 252 and the interval parameter set to `-1`
- **THEN** the firmware replies with `COMMAND_ACK`
- **AND** stops sending `NAMED_VALUE_INT` messages

#### Scenario: A ground station asks for the default rate

- **WHEN** a ground station sends `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id
  parameter set to 252 and the interval parameter set to `0`
- **THEN** the firmware replies with `COMMAND_ACK`
- **AND** does not begin sending `NAMED_VALUE_INT` messages

#### Scenario: The board resets after housekeeping was requested

- **WHEN** a ground station had requested housekeeping and the firmware then resets,
  for any reason
- **THEN** after the reset, no `NAMED_VALUE_INT` message is sent until a ground
  station requests it again

#### Scenario: Requested while in the reduced configuration

- **WHEN** the firmware is in the reduced configuration and a ground station sends
  `MAV_CMD_SET_MESSAGE_INTERVAL` with the message id parameter set to 252 and the
  interval parameter greater than zero
- **THEN** the firmware replies with `COMMAND_ACK` and begins sending
  `NAMED_VALUE_INT` messages exactly as in the normal configuration
