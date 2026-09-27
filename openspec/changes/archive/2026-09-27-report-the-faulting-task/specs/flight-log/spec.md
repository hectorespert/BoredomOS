# Spec Delta

## ADDED Requirements

### Requirement: The reset that started the board is recorded in the log

Every log file SHALL carry, from the point it is opened or reopened, a record of why the
running boot began: the cause of the previous reset, how far the previous boot
progressed, both fault counts as they stood at this boot, and — when the previous boot
ended in a fault handler — which fault it was and the name of the task it occurred in. The
causes, progress steps and fault kinds SHALL be drawn from the same sets the firmware
reports on the link, not a second enumeration.

A file opened more than once within one boot carries the same record again; the
cumulative count it holds identifies records that describe the same boot. Adding this
record SHALL NOT move the wall-clock record at the head of a file, so the time the log
listing reports for a file written before this record existed stays correct.

The record exists only while the log runs. A boot without a card, or in the reduced
configuration, writes none; the link's boot statement covers those boots.

The heartbeat and the boot statement report only the last reset. The log is what keeps
the history, so that a sequence of faults ending in the reduced configuration can be
reconstructed later.

#### Scenario: A log file records the boot that opened it

- **WHEN** the board boots in the normal configuration with a card present, and an
  operator later reads the file the log was writing, with a general-purpose flight-log tool
- **THEN** after the wall-clock record at the head of the file, or at the point where the
  file was reopened, there is a reset record whose cause and cumulative count match what
  the heartbeat reported during that boot

#### Scenario: A stack overflow is recorded with its task

- **WHEN** a task overflows its stack, the board resets, boots in the normal configuration
  with a card present, and an operator later reads the log
- **THEN** the reset record written by that boot names a stack overflow and the task that
  overflowed

#### Scenario: A file reopened within the same boot

- **WHEN** the log moves on to another file during one boot, and an operator reads both
  files
- **THEN** each carries a reset record, and both records hold the same cumulative count

#### Scenario: Files written before the reset record existed are still listed correctly

- **WHEN** the card holds a file written by a firmware without the reset record, and a
  ground station lists the logs
- **THEN** that file's `LOG_ENTRY` reports the same `time_utc` it reported before
