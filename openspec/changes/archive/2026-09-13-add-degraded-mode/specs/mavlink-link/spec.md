## MODIFIED Requirements

### Requirement: No task waits for the link port to become ready

No task SHALL block, spin or delay waiting for the link port to report itself ready
before producing or transmitting telemetry. The satellite SHALL operate with no host
and no ground station attached.

In the normal configuration, all tasks SHALL reach their steady-state cadence and
housekeeping records SHALL continue to be written to the SD card at 1 Hz. Whether that
holds in the reduced configuration, or with no SD card present, is governed by the
`fault-recovery` capability, not by this requirement.

#### Scenario: Board powered with nothing attached

- **WHEN** the board is powered with neither USB nor a ground station connected, and
  starts in the normal configuration
- **THEN** all tasks reach their steady-state cadence
- **AND** housekeeping records continue to be written to the SD card at 1 Hz

#### Scenario: Ground station attached after boot

- **WHEN** a ground station connects to the link some time after boot
- **THEN** it begins receiving telemetry at the normal rates without the board being
  reset
