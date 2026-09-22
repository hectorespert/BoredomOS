## ADDED Requirements

### Requirement: SYS_STATUS reports sensor presence, battery state and link error counters every second

Each port SHALL emit `SYS_STATUS` at 1000 ms, unconditionally — including in the
reduced configuration — alongside its `HEARTBEAT` and `SYSTEM_TIME` cadence. The
rate SHALL NOT be adjustable by `MAV_CMD_SET_MESSAGE_INTERVAL`, per the existing
requirement that a message id other than `NAMED_VALUE_INT` (252) is denied.

`onboard_control_sensors_present`, `_enabled` and `_health` SHALL carry the same
value for each bit this firmware reports: the SD card's presence in
`MAV_SYS_STATUS_LOGGING`, and the battery sense's presence, unconditionally set,
in `MAV_SYS_STATUS_SENSOR_BATTERY`. The real-time clock's presence SHALL NOT be
represented in these bitmaps; its absence remains reported only through
`STATUSTEXT`.

`voltage_battery` and `battery_remaining` SHALL report the same reading
`BATTERY_STATUS` reports at that moment, rather than the protocol's "not sent"
sentinel. `current_battery` SHALL report the protocol's "not sent" sentinel
(`-1`): this firmware has no current sensor, only the voltage ADC.

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

#### Scenario: A dropped outbound frame is counted

- **WHEN** a port's outbound queue is full and a frame for that port is dropped
  as a result
- **THEN** that port's next `SYS_STATUS` reports a higher `errors_count1` than
  its previous one
