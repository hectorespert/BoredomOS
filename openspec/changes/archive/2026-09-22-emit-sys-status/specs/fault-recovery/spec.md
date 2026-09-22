## MODIFIED Requirements

### Requirement: Absent hardware degrades rather than halts

Neither the real-time clock nor the SD card SHALL be required for the firmware to run.
When either is absent or does not respond, the firmware SHALL continue without it and
SHALL report its absence, rather than stopping.

Without a real-time clock, the firmware SHALL operate on time measured from boot, and
SHALL accept a time set by the ground. Without an SD card, the housekeeping record SHALL
be skipped and nothing else SHALL change.

The SD card's absence SHALL additionally be visible, with no request, to a ground
station that connects at any time after boot, through `SYS_STATUS`'s sensor-health
bitmap — the same "state without asking for it" guarantee the periodic heartbeat
already gives the reduced configuration. The real-time clock's absence has no
matching bit in that message and is not covered by this guarantee; it remains
reported only through the one-shot boot `STATUSTEXT`.

#### Scenario: The real-time clock does not respond

- **WHEN** the board boots with no real-time clock present
- **THEN** the firmware reaches its steady-state cadence
- **AND** a ground station sees the absence reported
- **AND** a time sent by the ground is accepted and used

#### Scenario: The card is missing

- **WHEN** the board boots with no SD card present
- **THEN** the firmware reaches its steady-state cadence
- **AND** a ground station sees the absence reported
- **AND** telemetry continues at its normal rates

#### Scenario: A ground station connects after the card was found absent

- **WHEN** the board booted with no SD card present and a ground station connects
  to either port afterward, having missed the boot `STATUSTEXT`
- **THEN** the next `SYS_STATUS` on that port already reports the card absent
- **AND** it required no request to obtain
