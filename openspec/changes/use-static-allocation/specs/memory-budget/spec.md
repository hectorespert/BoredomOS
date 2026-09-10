## Purpose

Defines where the firmware's RAM commitments live, which of them the build is
required to account for by name, what backs a declared queue depth, and how an
allocation that cannot be satisfied is made visible. It exists because the board has
32 KB of RAM, no way to report exhaustion from orbit, and no operator within reach of
the reset button.

## ADDED Requirements

### Requirement: Task and queue memory is accounted for at build time

The stack and control block of every task, and the structure and item storage of
every queue, SHALL live in storage that the build accounts for by name. A build whose
total RAM commitment exceeds what the device has SHALL fail and produce no firmware
image.

This is what makes the RAM figure a build reports meaningful: it MUST rise when a
task is added and fall when one is removed.

#### Scenario: A task is added that does not fit

- **WHEN** a task is added whose stack cannot be accommodated in the RAM that remains
- **THEN** the build fails
- **AND** it names the overflow and its size
- **AND** no firmware image is produced

#### Scenario: A task is added that does fit

- **WHEN** a task is added whose stack can be accommodated
- **THEN** the build succeeds
- **AND** the RAM figure it reports increases by that task's stack and control block

#### Scenario: Nothing is committed at run time that the build did not account for

- **WHEN** the firmware reaches its steady-state cadence
- **THEN** no task, task control block or queue structure has been created from memory
  the build did not account for by name

### Requirement: A declared queue depth is backed

Every queue SHALL be able to reach the depth it declares: whenever a queue is not
already full, memory to hold one more item SHALL be available. Declaring a depth that
the firmware cannot supply is not permitted, because it moves the failure from the
queue, where every producer handles it, to the allocator, where a failure is a null
pointer.

A producer that cannot hand over an item SHALL learn this from the queue.

#### Scenario: A queue is saturated by a burst

- **WHEN** a producer offers an item to a queue that already holds its declared depth
- **THEN** the producer is told the queue would not accept it
- **AND** the producer releases the item
- **AND** no allocation has failed

#### Scenario: Every queue is full at once

- **WHEN** every queue in the firmware simultaneously holds its full declared depth of
  items
- **THEN** every one of those items was obtained successfully
- **AND** memory remains for the firmware to continue running

### Requirement: An allocation that cannot be satisfied is signalled

WHEN a run-time allocation cannot be satisfied, the firmware SHALL make the condition
externally visible rather than failing silently.

The indication SHALL NOT depend on a host being attached, on a serial port having
been opened, or on interrupts remaining enabled — the conditions under which this
failure occurs are exactly the ones in which none of those can be relied upon.

#### Scenario: An allocation fails with nothing attached

- **WHEN** an allocation cannot be satisfied and no host is connected to the board
- **THEN** the board signals the condition by a means that is observable without a
  host

#### Scenario: An allocation fails and the indication is not swallowed

- **WHEN** an allocation cannot be satisfied
- **THEN** the indication is produced before control returns to the code that
  requested the memory

### Requirement: No RAM is committed to kernel facilities the firmware does not use

The firmware SHALL NOT reserve RAM for scheduler facilities it never invokes.

#### Scenario: Unused facilities hold no storage

- **WHEN** the build's memory map is inspected
- **THEN** it shows no storage reserved for a timer service, for per-task local
  storage slots, or for a registry of queue names
- **AND** the firmware invokes none of those facilities

### Requirement: Relocating the memory does not change observable behaviour

Moving where task, queue and item memory is reserved SHALL NOT change what the
firmware does: the same messages leave at the same rates with the same identity, the
housekeeping record keeps its 1 Hz cadence and its fields, and every task keeps the
stack depth it had.

#### Scenario: A ground station sees no difference

- **WHEN** a ground station is connected before and after the change
- **THEN** it receives the same message set at the same rates
- **AND** the vehicle identity is unchanged

#### Scenario: Stack depths are carried across unchanged

- **WHEN** the stack depth declared for each task is compared before and after
- **THEN** every one is identical
