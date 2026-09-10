## Purpose

Defines the text command interface the firmware answers on the USB console: which port
carries it, how a command is framed, what `ps`, `free` and `help` report, and the
guarantees the CLI owes the flight software so that a diagnostic tool can never be the
reason a task misses its deadline.

## ADDED Requirements

### Requirement: The console CLI is carried on the USB CDC port

The firmware SHALL accept text commands and emit text replies on the USB CDC port at
115200, and SHALL NOT emit MAVLink frames on it in the flight build. The console port
SHALL be named by a single definition that the whole firmware refers to, so that a
build may move the CLI to another port without editing any command implementation.

#### Scenario: Host opens the USB port on a flight build

- **WHEN** a host opens the USB CDC port at 115200 on a board running the flight build
- **THEN** it can send a command and receive a text reply
- **AND** it receives no MAVLink frames

#### Scenario: A build that puts the MAVLink link on USB

- **WHEN** the firmware is built with the MAVLink link moved onto the USB CDC port
- **THEN** the console port definition is overridden to the hardware UART
- **AND** no build carries both the MAVLink link and the CLI on the same port

### Requirement: The CLI is the console port's only writer while tasks run

The CLI SHALL be the only code that writes to the console port while the scheduler is
running. Any other subsystem needing to emit console text SHALL go through the CLI
rather than writing to the port directly.

#### Scenario: Firmware running normally

- **WHEN** the firmware is running and no command has been sent
- **THEN** the console emits nothing unsolicited
- **AND** replies appear only in response to a command

#### Scenario: A task overflows its stack

- **WHEN** the stack-overflow handler runs
- **THEN** it may write its diagnostic to the console directly
- **AND** this is not a second concurrent writer, because the scheduler has already
  stopped and no CLI command can be in progress

### Requirement: Commands are newline-terminated and answered before the next is read

A command SHALL be a line of text terminated by a newline. The firmware SHALL execute
one command at a time and finish emitting its reply before reading the next. An
unrecognised command SHALL produce an error line naming how to list the commands, and
SHALL leave the firmware unchanged.

#### Scenario: A recognised command

- **WHEN** a host sends a known command followed by a newline
- **THEN** the firmware emits that command's reply, then a prompt

#### Scenario: An unrecognised command

- **WHEN** a host sends text that matches no command
- **THEN** the firmware emits an error line pointing at `help`
- **AND** no firmware state changes

#### Scenario: An empty line

- **WHEN** a host sends a newline with no command
- **THEN** the firmware emits a prompt and nothing else

### Requirement: `ps` reports every task the kernel knows about

`ps` SHALL emit one row per task currently known to the scheduler, including tasks the
firmware did not create itself, such as the idle task and, where the kernel is built
with software timers, the timer-service task. The reply SHALL reflect whatever the
scheduler reports rather than a fixed roster: a kernel configuration that adds or
removes a task changes the output and nothing else. Each row
SHALL carry the task's identifier, its name, its priority, its scheduling state, and
the amount of stack it has never used, in the same units as the stack size given when
the task is created. The reply SHALL end with the free heap.

The task list SHALL be obtained from the scheduler rather than from a list maintained
alongside the CLI, so that a task added to the firmware later appears in `ps` with no
change to the CLI.

#### Scenario: `ps` on a running board

- **WHEN** a host sends `ps`
- **THEN** it receives one row per task, each naming the task, its priority, its state
  and its unused stack
- **AND** the rows include every task the scheduler knows about, among them tasks the
  firmware did not create, such as the idle task
- **AND** the last line reports free heap

#### Scenario: The kernel configuration changes which tasks exist

- **WHEN** the firmware is built with a kernel option that removes a task it does not
  create itself, such as disabling software timers
- **THEN** `ps` lists the tasks that remain and reports no error
- **AND** no CLI source file needed editing for the output to stay correct

#### Scenario: A task is added to the firmware

- **WHEN** a new task is created in the firmware and the board is reflashed
- **THEN** `ps` lists it
- **AND** no CLI source file needed editing for it to appear

#### Scenario: Stack figures are comparable to the configured sizes

- **WHEN** a reader compares a `ps` row against the stack size that task was created
  with, or against the same figure recorded in the SD log
- **THEN** both are in the same unit and can be compared without conversion

### Requirement: `ps` accepts a task name to report a single task

`ps` SHALL accept an optional task name and, when given one, SHALL emit the same row
format for that task alone. A name matching no task SHALL produce an error line and no
rows.

#### Scenario: A name that matches a task

- **WHEN** a host sends `ps` followed by the name of a running task
- **THEN** it receives the header and that task's row only

#### Scenario: A name that matches no task

- **WHEN** a host sends `ps` followed by a name no task has
- **THEN** it receives an error line saying no such task
- **AND** no task rows

### Requirement: `free` reports the heap

`free` SHALL report the total size of the heap tasks allocate from, how much of it is
free at that moment, and the smallest amount that has ever been free since boot.

#### Scenario: `free` on a running board

- **WHEN** a host sends `free`
- **THEN** it receives the heap total, the current free figure and the minimum ever
  free figure

#### Scenario: The low-water figure is monotonic

- **WHEN** `free` is sent twice with allocation activity in between
- **THEN** the minimum-ever-free figure never increases between the two replies

### Requirement: `help` lists the available commands

`help` SHALL list every command the firmware accepts with a one-line description of
each. It SHALL be reachable both as `help` and as `?`.

#### Scenario: Discovering the commands

- **WHEN** a host sends `help`, or sends `?`
- **THEN** it receives the same list, naming every command this capability defines

### Requirement: The CLI is read-only

No command SHALL change firmware state, configuration, or the behaviour of any other
task. The CLI SHALL expose observation only.

#### Scenario: A full command session during flight

- **WHEN** every command the CLI accepts is issued while the firmware is running
- **THEN** telemetry rates, logging, and timekeeping are unaffected
- **AND** nothing the ground can type through the console can leave the satellite in a
  different state than it found it

### Requirement: The CLI never delays a flight task

The CLI SHALL run at a priority below the tasks carrying telemetry, logging and the
link. Neither an absent host, nor a host that stops reading, nor a full transmit
buffer SHALL stall any other task or cause a periodic cadence to slip.

#### Scenario: No host attached

- **WHEN** the board runs with nothing connected to the console port
- **THEN** all periodic cadences hold
- **AND** the CLI consumes no more than its idle share of the processor

#### Scenario: Host stops reading mid-reply

- **WHEN** a host issues a command and then stops draining the port while a reply is
  being written
- **THEN** no other task is blocked
- **AND** the periodic telemetry and the SD log continue at their normal rates

#### Scenario: The CLI reports its own headroom

- **WHEN** `ps` is issued
- **THEN** the CLI's own task appears in the output with its unused stack
- **AND** that figure is the evidence used to size the CLI's stack
