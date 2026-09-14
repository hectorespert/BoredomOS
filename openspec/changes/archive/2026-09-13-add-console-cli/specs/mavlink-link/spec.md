## MODIFIED Requirements

### Requirement: The MAVLink link is carried on the hardware UART

The firmware SHALL exchange MAVLink frames over the hardware UART exposed on pins D0
(`RX`) and D1 (`TX`), at the link baud rate, and SHALL NOT exchange MAVLink frames
over the USB CDC port.

#### Scenario: Ground station attached to the UART pins

- **WHEN** a ground station is connected to D0/D1 at the link baud rate
- **THEN** it receives `HEARTBEAT` and `SYSTEM_TIME` at 1 Hz and `BATTERY_STATUS`
  every 2 s
- **AND** inbound `SYSTEM_TIME` and `TIMESYNC` sent on that port are acted upon

#### Scenario: Host attached to USB

- **WHEN** a host opens the USB CDC port at 115200
- **THEN** it receives no MAVLink frames
- **AND** it receives no unsolicited text; the console answers text commands, and the
  reservation the console previously held is now taken by the `console-cli` capability,
  which defines what it emits and when
