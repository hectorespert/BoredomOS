# Spec Delta

## MODIFIED Requirements

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

When the previous boot ended in a fault handler — a task overflowing its stack, or an
allocation that could not be satisfied — the firmware SHALL additionally know which of
the two it was and the name of the task that was running, as that task was named when it
was created. A name that cannot be read back intact SHALL be reported as far as it is
legible rather than as arbitrary bytes.

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

#### Scenario: A task overflows its stack

- **WHEN** a task overflows its stack and the board resets
- **THEN** the next boot reports a stack overflow
- **AND** it names the task that overflowed

#### Scenario: A fault is followed by an ordinary reset

- **WHEN** a boot that followed a fault-handler reset runs, and the board is then reset
  for an unrelated reason
- **THEN** the boot after that reports its own cause and does not repeat the earlier
  fault or its task

### Requirement: The ground can see the state without asking for it

The periodic heartbeat SHALL carry whether the firmware is in the reduced
configuration, the cause of the last reset, how far the last boot progressed, and both
counts — so that a ground station that connects at any time learns the state from the
next heartbeat, without issuing a request.

The firmware SHALL additionally report, once per boot and unasked, a human-readable
statement naming the suspected cause. When the previous boot ended in a fault handler,
the statement SHALL name which fault it was and the task it occurred in. The same
statement SHALL be obtainable on request at any time during the boot, on the port the
request arrives on, in every configuration, so that a ground station that was not
listening when the boot began can still read it.

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

#### Scenario: The boot statement names the faulting task

- **WHEN** a ground station is listening on the UART when the board boots after a stack
  overflow in a task
- **THEN** it receives a human-readable statement naming the reset cause, the stack
  overflow and that task

#### Scenario: The boot statement is requested after the boot

- **WHEN** a ground station attaches to either port after the boot, including over USB
  after a reset, and requests the boot statement
- **THEN** it receives the same statement the boot emitted, on that port only
