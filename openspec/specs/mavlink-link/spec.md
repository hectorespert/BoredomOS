# mavlink-link Specification

## Purpose
Defines which physical port carries the MAVLink link to the ground station, at what
speed, how that choice is made at build time, and what the firmware may assume about
the port being ready. The USB port's role as a text console is reserved here but not
yet used.

## Requirements

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
- **AND** it receives no text while the firmware is running normally; the console is
  reserved and its only writer is the stack-overflow handler, which runs only after
  the scheduler has stopped

### Requirement: The link port and baud rate are defined in one place

The link port and its baud rate SHALL each be named by a single definition that the
whole firmware refers to. Selecting a different port or speed SHALL be possible by
overriding those definitions at build time, without editing any file that implements
the MAVLink protocol or a task body.

#### Scenario: Reverting the link to USB for bench work

- **WHEN** the firmware is built with the link port overridden to the USB CDC port
- **THEN** the build succeeds with no other source change
- **AND** a ground station on the USB port sees the same messages, at the same rates,
  with the same identity as it did before this change

#### Scenario: Changing the link speed

- **WHEN** the firmware is built with the link baud rate overridden
- **THEN** the link operates at that speed and no other behaviour changes

### Requirement: No task waits for the link port to become ready

No task SHALL block, spin or delay waiting for the link port to report itself ready
before producing or transmitting telemetry. The satellite SHALL operate with no host
and no ground station attached.

#### Scenario: Board powered with nothing attached

- **WHEN** the board is powered with neither USB nor a ground station connected
- **THEN** all tasks reach their steady-state cadence
- **AND** housekeeping records continue to be written to the SD card at 1 Hz

#### Scenario: Ground station attached after boot

- **WHEN** a ground station connects to the link some time after boot
- **THEN** it begins receiving telemetry at the normal rates without the board being
  reset

### Requirement: Link traffic does not disturb periodic cadences

Transmitting MAVLink frames SHALL NOT shift the firmware's periodic cadences:
housekeeping sampling stays at 1 Hz and telemetry keeps its declared rates
regardless of link traffic.

This is a cadence guarantee, not a latency one. Transmission occupies the CPU for
the duration of a frame — a few milliseconds at link speed — and lower-priority
tasks do not run while it does. What SHALL hold is that this delay is absorbed
within each task's period rather than accumulating into drift.

#### Scenario: Sustained telemetry on a slow link

- **WHEN** the link is transmitting continuously at the link baud rate
- **THEN** housekeeping records are still written once per second
- **AND** `HEARTBEAT` and `SYSTEM_TIME` still leave at 1 Hz

### Requirement: The vehicle identity is unchanged by the move

Every outbound message SHALL keep system id `1`, component `MAV_COMP_ID_AUTOPILOT1`,
type `MAV_TYPE_ROCKET` and autopilot `MAV_AUTOPILOT_GENERIC`, on whichever port the
link is carried.

#### Scenario: Identity observed on either port

- **WHEN** a ground station is connected to the UART link, or to USB in the
  overridden build
- **THEN** it sees exactly one vehicle, system `1`, component
  `MAV_COMP_ID_AUTOPILOT1`, type `MAV_TYPE_ROCKET`
