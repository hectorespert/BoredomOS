## ADDED Requirements

### Requirement: MAVLink is carried on both the hardware UART and the USB port

The firmware SHALL exchange MAVLink frames over the hardware UART exposed on pins D0
(`RX`) and D1 (`TX`) at the link baud rate, and SHALL also exchange them over the USB
CDC port once that port has entered MAVLink mode.

The two are independent links to one vehicle, not one link duplicated. Each SHALL keep
its own frame parser state and its own outbound sequence numbering, and SHALL schedule
its own transmissions. A frame received on one SHALL NOT be forwarded to the other, and
a frame transmitted on one SHALL NOT imply a transmission on the other.

#### Scenario: Ground station on the UART pins

- **WHEN** a ground station is connected to D0/D1 at the link baud rate
- **THEN** it receives `HEARTBEAT` and `SYSTEM_TIME` at 1 Hz and `BATTERY_STATUS`
  every 2 s
- **AND** inbound `SYSTEM_TIME` and `TIMESYNC` sent on that port are acted upon

#### Scenario: Ground station on USB

- **WHEN** a ground station connects to the USB CDC port and sends a valid frame
- **THEN** it receives the same message set, at the same nominal rates, for as long as
  the port accepts the frames
- **AND** inbound `SYSTEM_TIME` and `TIMESYNC` sent on that port are acted upon
- **AND** the rate guarantee is weaker than the UART link's by design: the USB stream
  is produced at a lower priority and yields to it, so under load USB is the stream
  that slips

#### Scenario: Both ports occupied at once

- **WHEN** a ground station is attached to each port simultaneously
- **THEN** each receives a complete, independently sequenced telemetry stream
- **AND** neither sees frames that were sent to or by the other

#### Scenario: The message set changes

- **WHEN** an outbound message's contents change, or an inbound message gains a
  handler
- **THEN** both ports reflect the change together
- **AND** neither port can be left emitting or accepting a different set than the other

#### Scenario: One port idle

- **WHEN** nothing is attached to one of the two ports
- **THEN** the other continues at its normal rates
- **AND** no task is delayed by the idle port

### Requirement: The USB transmit path allocates nothing and never blocks

Transmitting a frame on the USB port SHALL NOT allocate from the heap and SHALL NOT
wait for the port to drain. Before writing, the firmware SHALL check that the port can
accept the whole frame; when it cannot, the frame SHALL be dropped and retried on a
later pass rather than queued or waited on. At most one frame SHALL be transmitted per
scheduling pass, and pending inbound bytes SHALL be consumed before telemetry is
emitted.

#### Scenario: Host stops draining the USB port

- **WHEN** a host opens the USB port, provokes MAVLink mode and then stops reading
- **THEN** no task blocks and no periodic cadence slips
- **AND** the undelivered frames are dropped rather than accumulating

#### Scenario: A reply is due while telemetry is scheduled

- **WHEN** an inbound frame requiring a reply arrives in the same pass as scheduled
  telemetry
- **THEN** the inbound bytes are consumed first
- **AND** the reply is not starved by the telemetry schedule

### Requirement: The link baud rate is defined in one place

The hardware UART link's baud rate SHALL be named by a single definition that the
whole firmware refers to, and SHALL be selectable at build time without editing any
file that implements the MAVLink protocol or a task body.

The ports themselves SHALL NOT be selectable: the UART carries the link and the USB
port carries the console and MAVLink, in every build.

#### Scenario: Changing the link speed

- **WHEN** the firmware is built with the link baud rate overridden
- **THEN** the UART link operates at that speed and no other behaviour changes

#### Scenario: No build moves a protocol to another port

- **WHEN** any environment in the build configuration is built
- **THEN** the UART carries MAVLink and the USB port carries the console and MAVLink
- **AND** no build flag exists that relocates either

### Requirement: The vehicle identity is the same on both ports

Every outbound message SHALL carry system id `1`, component `MAV_COMP_ID_AUTOPILOT1`,
type `MAV_TYPE_ROCKET` and autopilot `MAV_AUTOPILOT_GENERIC`, on both ports. The two
links SHALL present one vehicle, not two, and SHALL number their sequences
independently of one another.

#### Scenario: Identity observed on either port

- **WHEN** a ground station is connected to the UART link, or to USB
- **THEN** it sees exactly one vehicle, system `1`, component
  `MAV_COMP_ID_AUTOPILOT1`, type `MAV_TYPE_ROCKET`

#### Scenario: Sequence numbers do not interfere

- **WHEN** both ports are streaming and one of them drops frames because its host
  stopped reading
- **THEN** the other port's sequence numbering shows no gaps caused by those drops

## MODIFIED Requirements

### Requirement: No task waits for the link port to become ready

No task SHALL block, spin or delay waiting for either port to report itself ready
before producing or transmitting telemetry. In particular, the firmware SHALL NOT wait
on the USB CDC port becoming enumerated. The satellite SHALL operate with no host and
no ground station attached to either port.

#### Scenario: Board powered with nothing attached

- **WHEN** the board is powered with neither USB nor a ground station connected
- **THEN** all tasks reach their steady-state cadence
- **AND** housekeeping records continue to be written to the SD card at 1 Hz

#### Scenario: Ground station attached after boot

- **WHEN** a ground station connects to either port some time after boot
- **THEN** it begins receiving telemetry at the normal rates without the board being
  reset

#### Scenario: USB never enumerated in flight

- **WHEN** the satellite runs with no USB host for its whole mission
- **THEN** the USB port stays in console mode, emits nothing, and costs no cadence

### Requirement: Link traffic does not disturb periodic cadences

Transmitting MAVLink frames on either port SHALL NOT shift the firmware's periodic
cadences: housekeeping sampling stays at 1 Hz and telemetry keeps its declared rates
regardless of traffic on either link.

This is a cadence guarantee, not a latency one. Transmission occupies the CPU for
the duration of a frame — a few milliseconds at link speed — and lower-priority
tasks do not run while it does. What SHALL hold is that this delay is absorbed
within each task's period rather than accumulating into drift.

#### Scenario: Sustained telemetry on a slow link

- **WHEN** the UART link is transmitting continuously at the link baud rate
- **THEN** housekeeping records are still written once per second
- **AND** `HEARTBEAT` and `SYSTEM_TIME` still leave at 1 Hz

#### Scenario: Both links saturated

- **WHEN** both ports are transmitting continuously
- **THEN** the housekeeping cadence and the UART link's rates are unchanged from when
  USB was idle
- **AND** the USB stream degrades by dropping frames, never by delaying another task

## REMOVED Requirements

### Requirement: The MAVLink link is carried on the hardware UART

**Reason**: The requirement forbade MAVLink on the USB CDC port and made the port a
build-time choice. This change makes USB a second, permanent MAVLink endpoint, so the
prohibition and the build-time selection both cease to hold.

**Migration**: Replaced by *MAVLink is carried on both the hardware UART and the USB
port*, which keeps the UART behaviour verbatim and adds the USB endpoint. Builds that
used `-D LINK_SERIAL=Serial` to reach the link over USB no longer need it: every build
answers MAVLink on USB. The `bench` environment that carried that override is deleted.

### Requirement: The link port and baud rate are defined in one place

**Reason**: Its "Reverting the link to USB for bench work" scenario existed so that a
build could move the MAVLink link onto USB when no USB-TTL adapter was available. USB
now carries MAVLink unconditionally, so there is nothing to revert and no port left to
select — only the baud rate remains a build-time choice.

**Migration**: Replaced by *The link baud rate is defined in one place*, which keeps
the baud-rate behaviour verbatim and states that the ports are fixed. `-D LINK_SERIAL`
is removed along with the `bench` environment; anyone who used it to reach the link
over USB now simply attaches to USB and sends a frame.

### Requirement: The vehicle identity is unchanged by the move

**Reason**: The requirement was written when exactly one port carried the link and the
question was whether moving it changed the vehicle's identity. With two simultaneous
endpoints the question is no longer about a move but about two links agreeing, and the
requirement has to say something about sequence numbering that it never did.

**Migration**: Replaced by *The vehicle identity is the same on both ports*, which
keeps the identity triple and the autopilot field verbatim, requires them on both
ports at once, and adds that the two links number their sequences independently.
