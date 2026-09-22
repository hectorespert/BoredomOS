## ADDED Requirements

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
