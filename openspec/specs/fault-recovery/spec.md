# fault-recovery Specification

## Purpose

Defines how the firmware survives its own faults without an operator: how a task that
stops yielding becomes a reset, how a reset is explained rather than guessed at, how repeated failures
select a reduced configuration, what that configuration must still be able to do, and
how all of it reaches the ground. It exists because there is no RESET button in orbit,
and a fault that is present at boot is otherwise permanent.

## Requirements

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

The firmware SHALL determine, at each boot, whether the previous reset was a power-on, a
low-voltage condition, a watchdog reset, a software reset, or none of these — in which
case it SHALL be reported as an external or unknown reset, inferred rather than read
from a dedicated flag. The firmware SHALL clear the record it read so the next boot
reads its own cause and not an accumulation, except where clearing a flag would conflict
with a use the flag already has outside this firmware; such a case SHALL be resolved
before it is implemented, not left to whichever behaviour the clear happens to produce.

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

#### Scenario: The supply voltage sags

- **WHEN** the previous reset was caused by a low-voltage condition rather than an
  ordinary power-on
- **THEN** the next boot identifies the cause distinctly from a power-on

#### Scenario: The RESET pin is pressed

- **WHEN** the board is reset by the RESET pin, with no power-on, watchdog, software or
  low-voltage cause present
- **THEN** the next boot identifies the cause as external or unknown, rather than as one
  of the other four

### Requirement: Repeated failure selects a reduced configuration

The firmware SHALL count boots that did not reach stability, and SHALL keep a separate
cumulative count of resets that only a ground command clears. When either count passes
its threshold, the firmware SHALL start in a reduced configuration instead of the normal
one.

A boot SHALL be declared stable only after it has run for longer than the failure it is
meant to catch: a fault that appears minutes after boot must not be recorded as a
success. The cumulative count exists for exactly that case, where the consecutive count
is cleared each time and never reaches its threshold.

A power-on, a low-voltage reset or an external reset SHALL be recorded but SHALL NOT
advance the consecutive count: those are events with an operator or the power supply
behind them, not evidence of a fault. A software reset that the firmware performs
deliberately — to leave the reduced configuration, whether by ground command or by the
automatic retry below — SHALL likewise be recorded but SHALL NOT advance the consecutive
count, and SHALL be distinguishable from a software reset that follows a fault.

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

#### Scenario: The board leaves the reduced configuration by its own action

- **WHEN** the firmware resets itself deliberately, whether by ground command or by the
  automatic retry, in order to leave the reduced configuration
- **THEN** that reset does not advance the consecutive count
- **AND** it is distinguishable, at the next boot, from a reset that followed a fault

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
a firmware whose link is the failing part is not stranded in it permanently. Because the
automatic retry's own reset is itself recorded in the cumulative count, evaluating that
count against its raw threshold on the retry boot would find it still exceeded and
return to reduced before ever giving the retry a chance — the interval would then select
the reduced configuration on every single attempt, forever. To leave that possible, the
firmware SHALL record the cumulative count at the moment it enters the reduced
configuration, and the boot that follows the automatic retry SHALL compare the
cumulative count at that boot against the recorded value rather than against the raw
threshold: since the retry's own reset predictably advances the cumulative count by
one, it SHALL start in the normal configuration unless the cumulative count has grown
by more than that one expected increment since the reduced configuration was entered.

The retry interval SHALL be long enough that a board failing repeatedly does not
oscillate rapidly between configurations, measured as the interval between successive
entries into the reduced configuration being at least the retry interval.

#### Scenario: The ground commands a return to normal

- **WHEN** the ground sends the reboot command while the firmware is in the reduced
  configuration
- **THEN** the counts are cleared
- **AND** the board restarts in the normal configuration

#### Scenario: The link is the failing part

- **WHEN** the firmware has been in the reduced configuration for longer than the retry
  interval, has received no command, and no new fault has occurred since the reduced
  configuration was entered
- **THEN** it restarts in the normal configuration

#### Scenario: The fault that caused the reduced configuration is still present

- **WHEN** the firmware automatically retries the normal configuration and the fault
  that caused the reduced configuration recurs
- **THEN** it returns to the reduced configuration
- **AND** the interval between that entry and the previous one is at least the retry
  interval
