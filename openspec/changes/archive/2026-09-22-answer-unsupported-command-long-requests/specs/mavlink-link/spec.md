## ADDED Requirements

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
