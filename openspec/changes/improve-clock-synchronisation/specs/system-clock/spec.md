## Purpose

Governs the time the satellite reports: how precisely it is known, where it came from,
which source of a time outranks which, what the firmware will accept from the ground,
and the continuity of time measured since the board started.

## ADDED Requirements

### Requirement: Reported UNIX time resolves below one second

Where the firmware reports UNIX time in a field finer than seconds, the value SHALL
carry a sub-second part derived from the same clock that advances the second, so that
the reported time is not systematically behind the true time. Successive readings
within one second SHALL differ, and the sub-second part SHALL NOT decrease except at a
second boundary.

#### Scenario: A ground station samples the reported time repeatedly

- **WHEN** a ground station collects consecutive `SYSTEM_TIME` messages from a board
  whose clock has been set
- **THEN** the sub-second part of `time_unix_usec` is not always zero
- **AND** across the sequence the value never decreases

### Requirement: Time measured since boot never goes backwards

The firmware SHALL maintain a measure of time elapsed since the board started that
increases monotonically for as long as the board runs, and SHALL NOT let a correction
to the wall clock disturb it. Setting the wall clock forwards or backwards SHALL leave
the elapsed measure continuous across the correction.

#### Scenario: The ground corrects the clock backwards

- **WHEN** a ground station reads the vehicle's elapsed time, then sets the wall clock
  to a plausible time earlier than the one the vehicle was reporting, then reads the
  elapsed time again
- **THEN** the second reading is greater than the first
- **AND** the wall clock the vehicle reports afterwards is the one that was set

#### Scenario: A ground station measures the link offset

- **WHEN** a ground station sends `TIMESYNC` with `tc1` set to zero
- **THEN** the answer's `tc1` is the vehicle's elapsed time since boot in nanoseconds,
  not its wall clock
- **AND** it corresponds to the moment the request was received rather than the moment
  the answer was emitted

### Requirement: The provenance of the current time is known and ranked

The firmware SHALL know which of four origins the time it holds came from: set by the
ground, seeded from the battery-backed clock, inherited from a clock that was already
running when this boot began, or none. A time from the ground SHALL outrank one from
the battery-backed clock, and any of the three SHALL outrank none. A time SHALL NOT be
replaced by one from a lower-ranked origin.

#### Scenario: A ground station learns the origin at boot

- **WHEN** a ground station is attached when the board starts
- **THEN** the boot report names which of the four origins the clock has

#### Scenario: The ground supersedes the battery-backed clock

- **WHEN** a board that seeded its clock from the battery-backed clock is sent a
  plausible time from the ground
- **THEN** the reported time becomes the one the ground sent
- **AND** the board reports that the origin is now the ground

### Requirement: A time offered by the ground is accepted only if plausible

The firmware SHALL reject an offered time that falls before a fixed cut-off date, and
SHALL leave its own clock and the battery-backed clock untouched when it does. An
accepted time SHALL be written to both clocks, so that the clock which seeds the next
boot is corrected too.

#### Scenario: An implausible time is offered

- **WHEN** a ground station sends a time of zero, or any time before the cut-off date
- **THEN** the time the vehicle reports afterwards is the one it was reporting before

#### Scenario: A plausible time is offered

- **WHEN** a ground station sends a plausible time
- **THEN** the time the vehicle reports afterwards matches it
- **AND** the time reported after the next restart still matches it, without the ground
  sending it again

### Requirement: An unknown clock is reported as unknown

When the firmware has no origin for its clock, it SHALL report its UNIX time as zero
rather than as a small number that reads as a date in 1970, and SHALL continue to
report time since boot normally.

#### Scenario: The board runs with no clock set

- **WHEN** a ground station attaches to a board with no battery-backed clock present
  that has not been sent a time
- **THEN** the `time_unix_usec` it receives is zero
- **AND** the `time_boot_ms` it receives advances between messages

#### Scenario: A clock is set on a board with no battery-backed clock

- **WHEN** that board is then sent a plausible time from the ground
- **THEN** the `time_unix_usec` it reports is no longer zero and matches the time sent

### Requirement: A clock set from the ground outlives a reset

A restart that leaves the board powered SHALL NOT discard the time the board was
holding. After such a restart the firmware SHALL continue from that time when nothing
better seeded it, and SHALL report the origin as inherited in that case.

The boot report SHALL state, separately from the origin it selected, whether a clock was
already running with a plausible time when the firmware started. That statement SHALL
NOT depend on which origin won, so that the fact remains observable on a board whose
battery-backed clock is always present.

#### Scenario: The board is commanded to restart

- **WHEN** a ground station sets a plausible time, commands a restart, and reads the boot
  report after the board comes back
- **THEN** the report states that a clock was found already running with a plausible time
- **AND** the time reported afterwards is not zero and is no earlier than the time that
  was set

#### Scenario: The board is powered on from cold

- **WHEN** the board is powered on after being fully unpowered, with no battery-backed
  clock present
- **THEN** the boot report states that no running clock was found
- **AND** the origin it names is not inherited
