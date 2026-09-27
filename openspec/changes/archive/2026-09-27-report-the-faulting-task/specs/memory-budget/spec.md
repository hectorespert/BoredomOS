# Spec Delta

## MODIFIED Requirements

### Requirement: An allocation that cannot be satisfied stops the firmware

When a run-time allocation cannot be satisfied, the firmware SHALL record the condition,
together with the task that asked for the memory, where the next boot can read it, and
SHALL then stop and reset itself: no further task runs between the failure and the reset.
If the failure occurs before any task has started, the record SHALL say so instead of
naming a task.

Signalling without stopping is not sufficient. The condition is detected in the
context of whichever task asked for the memory, with the scheduler still running, so a
firmware that merely signals would keep transmitting telemetry from tasks of higher
priority while it is out of memory — a state indistinguishable, from the ground, from
one that is working.

Recording and resetting SHALL depend on no host being attached, no serial port having
been opened, and no interrupt or timekeeping remaining serviceable — the conditions under
which this failure occurs are exactly the ones in which none of those can be relied
upon. The failure SHALL be reported to the ground at the next boot as the `fault-recovery`
capability reports any reset, distinguishably from a stack overflow.

The board's own indicator SHALL NOT be used for this. It is shared with the SD card's
clock, so no indication could persist while the board runs, and the board resets well
before any pattern could be read.

#### Scenario: An allocation fails with nothing attached

- **WHEN** an allocation cannot be satisfied in a task and no host is connected to the
  board
- **THEN** the board resets itself
- **AND** a ground station that attaches after the reset is told that the previous reset
  followed an allocation failure, not a stack overflow, and in which task

#### Scenario: The firmware does not continue after signalling

- **WHEN** an allocation cannot be satisfied
- **THEN** no further telemetry leaves the link before the board resets
- **AND** no further housekeeping record is written before the board resets

#### Scenario: An allocation fails before any task has started

- **WHEN** an allocation cannot be satisfied during initialisation, before the first task
  runs
- **THEN** the board resets itself
- **AND** the next boot reports an allocation failure without naming a task
