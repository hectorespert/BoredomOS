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
