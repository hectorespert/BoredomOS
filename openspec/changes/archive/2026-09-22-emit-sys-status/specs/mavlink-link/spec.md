## ADDED Requirements

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

## MODIFIED Requirements

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
