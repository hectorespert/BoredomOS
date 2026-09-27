# memory-budget Specification

## Purpose
Defines where the firmware's RAM commitments live, which of them the build is
required to account for individually, what backs a declared queue depth, and how an
allocation that cannot be satisfied is made visible. It exists because the board has
32 KB of RAM, no way to report exhaustion from orbit, and no operator within reach of
the reset button.

## Requirements

### Requirement: Task and queue memory is accounted for at build time

The RAM total that the build reports SHALL include every task stack, every task
control block and every queue structure individually, so that the total rises when a
task is added and falls when one is removed.

A build whose total RAM commitment exceeds what the device has, or leaves less than a
declared minimum of headroom, SHALL fail with a non-zero result and report the
shortfall in bytes. It SHALL NOT be necessary to read the linker's own diagnostic to
learn the size.

#### Scenario: A task is added that does not fit

- **WHEN** a task is created whose stack cannot be accommodated in the RAM that
  remains
- **THEN** the build fails with a non-zero result
- **AND** it identifies the RAM overrun and the number of bytes by which the commitment
  exceeds what is available
- **AND** any image left on disk by a partially successful build is reported as
  over-budget rather than treated as valid

#### Scenario: A task is added that does fit

- **WHEN** a task is created whose stack can be accommodated
- **THEN** the build succeeds
- **AND** the RAM total it reports increases by at least that task's stack and control
  block

#### Scenario: Every kernel object appears in the built image

- **WHEN** the built image's symbol table and memory map are inspected
- **THEN** every task stack, every task control block and every queue structure appears
  as its own named object within the RAM total the build reports
- **AND** the pool from which memory is obtained at run time is committed to queued
  items only

### Requirement: A declared queue depth is backed

Every queue SHALL be able to reach the depth it declares: whenever a queue is not
already full, memory to hold one more item SHALL be obtainable. Declaring a depth the
firmware cannot supply is not permitted, because it moves the failure from the queue,
where every producer handles it, to the point of allocation, where it is not handled.

The memory reserved SHALL account for the items that exist without sitting in a queue:
the item a producer is holding when it discovers the queue is full, the item a
consumer holds between taking it and releasing it, and one such item for every
producer that can be doing this at the same instant.

A producer whose item the queue did not accept SHALL release that item's memory.

#### Scenario: A queue is saturated by a burst

- **WHEN** a producer offers an item to a queue that already holds its declared depth
- **THEN** the producer is told the queue would not accept it
- **AND** the producer releases the item
- **AND** no allocation has failed

#### Scenario: Every queue is full at once

- **WHEN** every queue in the firmware simultaneously holds its full declared depth,
  every producer is holding an item it has not yet handed over, and every consumer is
  holding an item it has not yet released
- **THEN** every one of those items was obtained successfully
- **AND** the housekeeping cadence and the telemetry rates are unchanged

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

### Requirement: No RAM is committed to kernel facilities the firmware does not use

The firmware SHALL NOT reserve RAM for a scheduler facility that no code in the
firmware invokes.

#### Scenario: An unused facility is found to hold storage

- **WHEN** the built image is audited for storage belonging to a scheduler facility
- **THEN** every such facility found holding storage is one the firmware invokes

### Requirement: Where memory is reserved is not externally observable

Whether a task stack, a queue structure or a queued item is reserved when the image is
linked or obtained while the firmware runs SHALL NOT be observable in the firmware's
external behaviour. In the normal configuration, the message set, rates and identity
are those the `mavlink-link` capability defines, and the housekeeping record keeps its
cadence and its fields. The reduced configuration defined by the `fault-recovery`
capability is a distinct, observable state and is not governed by this requirement.

This governs steady-state behaviour. What a producer observes when a queue is
saturated is governed by *A declared queue depth is backed*, and is deliberately
different.

#### Scenario: A ground station sees no difference

- **WHEN** a ground station is connected to the link and the firmware is in the normal
  configuration
- **THEN** it receives the message set, at the rates and with the identity that the
  `mavlink-link` capability defines
- **AND** nothing in that traffic reveals where the firmware's memory was reserved
