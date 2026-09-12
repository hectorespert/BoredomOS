## Purpose

Defines how the firmware survives its own faults without an operator: how a task that
stops yielding becomes a reset, how a reset is explained rather than guessed at, how repeated failures
select a reduced configuration, what that configuration must still be able to do, and
how all of it reaches the ground. It exists because there is no RESET button in orbit,
and a fault that is present at boot is otherwise permanent.

## ADDED Requirements

### Requirement: A task that stops yielding resets the board

The firmware SHALL reset itself when no task yields for longer than a declared timeout.
The timeout SHALL exceed the slowest legitimate cycle in the firmware, so that normal
operation never triggers it.

A task that is merely slow SHALL NOT cause a reset. A task that is blocked forever, or
spinning without yielding, SHALL.

#### Scenario: A task blocks forever

- **WHEN** a task stops yielding and does not resume
- **THEN** the board resets within the declared timeout
- **AND** the reset is recorded as caused by the watchdog

#### Scenario: The slowest legitimate cycle completes

- **WHEN** the firmware runs normally, including its slowest operation
- **THEN** no reset occurs

#### Scenario: A fault handler is reached

- **WHEN** execution reaches a handler that signals a fault and does not return
- **THEN** the board resets rather than remaining in that handler indefinitely

### Requirement: The cause of a reset is known at the next boot

The firmware SHALL determine, at each boot, whether the previous reset was a power-on,
a watchdog reset, a software reset, or an external reset, and SHALL clear that record so
the next boot reads its own cause and not an accumulation.

The firmware SHALL also record how far the previous boot progressed, in enough detail to
name which initialisation step was in progress when it stopped.

#### Scenario: The board resets after a hang

- **WHEN** the watchdog resets the board
- **THEN** the next boot identifies the cause as the watchdog
- **AND** the boot after that does not also report a watchdog reset unless one occurred

#### Scenario: Initialisation stops partway

- **WHEN** a boot stops during an initialisation step and the board resets
- **THEN** the next boot can name the step that was in progress

#### Scenario: The board is powered on

- **WHEN** the board is powered on rather than reset
- **THEN** the boot identifies the cause as a power-on

### Requirement: Repeated failure selects a reduced configuration

The firmware SHALL count boots that did not reach stability, and SHALL keep a separate
cumulative count of resets that only a ground command clears. When either count passes
its threshold, the firmware SHALL start in a reduced configuration instead of the normal
one.

A boot SHALL be declared stable only after it has run for longer than the failure it is
meant to catch: a fault that appears minutes after boot must not be recorded as a
success. The cumulative count exists for exactly that case, where the consecutive count
is cleared each time and never reaches its threshold.

A power-on or an external reset SHALL be recorded but SHALL NOT advance the consecutive
count: those are events with an operator behind them, not evidence of a fault.

#### Scenario: Three consecutive boots fail

- **WHEN** three boots in a row reset before reaching stability
- **THEN** the next boot starts in the reduced configuration

#### Scenario: A fault appears after every boot has been declared stable

- **WHEN** the board resets repeatedly, each time after having been declared stable
- **THEN** the cumulative count still reaches its threshold
- **AND** the firmware starts in the reduced configuration

#### Scenario: A boot runs normally

- **WHEN** a boot reaches stability and keeps running
- **THEN** the consecutive count is cleared
- **AND** the next boot starts in the normal configuration

### Requirement: The reduced configuration stays reachable and commandable

The reduced configuration SHALL keep the firmware able to transmit on the link, to
receive on the link, and to act on what it receives. It SHALL NOT depend on the SD card,
the real-time clock or the battery sense.

Receiving SHALL NOT be dropped from the reduced configuration under any circumstances: a
state the ground cannot command its way out of is a trap, not a safe mode.

#### Scenario: The board boots reduced with a ground station attached

- **WHEN** the firmware starts in the reduced configuration and a ground station is
  connected to the link
- **THEN** it receives heartbeats
- **AND** a command sent to the board is acted upon

#### Scenario: The card and the clock are both absent in the reduced configuration

- **WHEN** the firmware starts in the reduced configuration with no SD card and no
  real-time clock present
- **THEN** it still transmits and still receives

### Requirement: Absent hardware degrades rather than halts

Neither the real-time clock nor the SD card SHALL be required for the firmware to run.
When either is absent or does not respond, the firmware SHALL continue without it and
SHALL report its absence, rather than stopping.

Without a real-time clock, the firmware SHALL operate on time measured from boot, and
SHALL accept a time set by the ground. Without an SD card, the housekeeping record SHALL
be skipped and nothing else SHALL change.

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

### Requirement: The ground can see the state without asking for it

The periodic heartbeat SHALL carry whether the firmware is in the reduced
configuration, the cause of the last reset, how far the last boot progressed, and both
counts — so that a ground station that connects at any time learns the state from the
next heartbeat, without issuing a request.

The firmware SHALL additionally report, once per boot, a human-readable statement naming
the suspected cause.

This SHALL NOT change the vehicle identity, the message set, or the stream rates that
the `mavlink-link` capability defines.

#### Scenario: A ground station connects long after a degraded boot

- **WHEN** a ground station connects to the link while the firmware is in the reduced
  configuration
- **THEN** the first heartbeat it receives already reports the reduced state, the reset
  cause, the phase reached and both counts
- **AND** it has issued no request to obtain them

#### Scenario: Normal operation is not reported as degraded

- **WHEN** the firmware is running normally
- **THEN** the heartbeat reports it as operational
- **AND** the identity, message set and rates are those the `mavlink-link` capability
  defines

### Requirement: The reduced configuration can be left

The firmware SHALL return to the normal configuration on a ground command that clears
the counts and restarts it.

It SHALL also leave the reduced configuration on its own after a long interval, so that
a firmware whose link is the failing part is not stranded in it permanently. That
interval SHALL be long enough that a board failing repeatedly does not oscillate between
configurations.

#### Scenario: The ground commands a return to normal

- **WHEN** the ground sends the reboot command while the firmware is in the reduced
  configuration
- **THEN** the counts are cleared
- **AND** the board restarts in the normal configuration

#### Scenario: The link is the failing part

- **WHEN** the firmware has been in the reduced configuration for longer than the retry
  interval and has received no command
- **THEN** it restarts in the normal configuration
- **AND** if it fails again it returns to the reduced configuration rather than looping
  rapidly between the two
