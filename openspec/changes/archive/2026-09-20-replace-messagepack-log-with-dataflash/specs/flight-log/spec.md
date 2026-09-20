## Purpose

Defines what the flight log guarantees to whoever eventually reads it. The log is the
only record of how close a 96-word task came to overflowing, it is written to a card
that will be pulled out and read on another machine, and the expected way for it to end
is the power dying mid-write. This capability states what survives that, what a reader
needs no project-specific software for, and what a recorded figure is allowed to mean.

## ADDED Requirements

### Requirement: The flight log is readable without software written for this project

The log SHALL be stored in a format that general-purpose flight-log tools parse, and a
reader SHALL be able to recover every recorded field, with its name, without writing
or obtaining any code specific to this firmware.

A format that requires a bespoke reader has the same practical value as no log at all:
the card is read months later, by someone who has to trust the numbers in it to decide
whether a stack is about to overflow, and a private encoding makes that a
reverse-engineering exercise at exactly the wrong moment.

#### Scenario: A card is read on a machine that has never seen this project

- **WHEN** an operator removes the card, copies a log file to a host, and parses it
  with a general-purpose flight-log tool
- **THEN** each record is presented with its field names and every recorded value is
  recoverable, with no project-specific parser involved

### Requirement: Each log file is readable on its own

Every log file SHALL carry the definition of each record type it contains, ahead of the
first record of that type, so that a file separated from its siblings remains fully
parseable. A file that is reopened and appended to SHALL carry those definitions again
from the point of reopening.

The ring reuses file names and a mission spans power cycles, so a reader has no
guarantee of holding the whole set, and no guarantee that the file it holds was written
in one sitting.

#### Scenario: One file of several is parsed alone

- **WHEN** an operator copies a single log file off a card that holds several and
  parses it without the others present
- **THEN** every record in it is decoded with its field names, and nothing in the file
  refers to a definition that is not in it

#### Scenario: A file that was appended to after a power cycle

- **WHEN** a log file that the firmware opened, wrote to, lost power on, and reopened
  on the next boot is parsed
- **THEN** the records written after the reopening decode as fully as those written
  before it

### Requirement: A truncated record costs only that record

A log file whose last record is incomplete SHALL remain parseable up to the truncation,
and a reader SHALL be able to resume at the next complete record following an
unreadable stretch anywhere in the file.

Power loss mid-write is the normal ending, not an exceptional one, and a boot that
reopens the same file puts the damaged stretch in the middle rather than at the end.

#### Scenario: Power is removed while a record is being written

- **WHEN** power is removed from the board while the log is being written, and the card
  is then read
- **THEN** every record completed before the interruption is decoded, and the
  incomplete tail is the only data lost

#### Scenario: A damaged stretch sits in the middle of a file

- **WHEN** a file containing an unreadable stretch followed by further complete records
  is parsed
- **THEN** the records after the damaged stretch are decoded, rather than the remainder
  of the file being lost

### Requirement: A recorded figure comes from a reading

The log SHALL record a measured quantity only when that quantity was actually read. The
absence of a measurement SHALL be represented by the absence of a record, never by a
record carrying zero or any other substitute value.

A field that reads zero whether the sensor was absent, unread or genuinely at zero
cannot be used for anything, and is worse than a gap because it looks like data.

#### Scenario: The log's battery figures are checked against the link

- **WHEN** a ground station records the battery telemetry the board reports over the
  link, and an operator then reads the log covering the same period
- **THEN** the battery figures in the log agree with what the link reported, and no
  recorded battery figure reads zero for a period in which the link reported a
  non-zero one

### Requirement: The log records the stack headroom of every task that can exist

The log SHALL record, at a fixed cadence of once per second, the free heap and the
unused stack remaining for each task the firmware can run, including those a reduced
configuration does not start.

This is the reason the log exists. Sizing a stack has no other source of evidence, and
a task absent from the record is a task whose margin cannot be checked after changing
its body.

#### Scenario: Stack figures in the log agree with the link

- **WHEN** a ground station arms the housekeeping telemetry stream and records the
  stack figures it reports, and an operator then reads the log covering the same period
- **THEN** the log carries one record per second, each holding a figure for every task
  that can exist, and those figures agree with the ones the link reported

### Requirement: A change of the wall clock is visible in the log

When the firmware accepts a wall clock from outside itself, the log SHALL record the
newly held time together with the origin it came from. The origin recorded SHALL be
drawn from the same set of origins the firmware reports elsewhere, not a second
enumeration.

Without this, a mid-mission clock correction appears in the log as an unexplained jump
in timestamps, and a timestamp produced while the clock was unknown is
indistinguishable from one produced while it was trustworthy.

#### Scenario: A ground station sets the clock mid-flight

- **WHEN** a ground station sends a time that the firmware accepts, and an operator
  then reads the log
- **THEN** the log contains a record at that point giving the new wall clock and naming
  the origin as the ground

#### Scenario: A log file is opened

- **WHEN** an operator reads any log file
- **THEN** the file carries the wall clock and its origin as held at the moment the file
  was opened, so records in it can be placed on an absolute timeline

### Requirement: The log's time reference does not wrap within a mission

Each record SHALL carry a time reference that increases for the whole duration of a
mission measured in months, without wrapping and without ambiguity between two
different instants.

#### Scenario: The board runs past the point a 32-bit millisecond counter would wrap

- **WHEN** the board has run continuously for longer than a 32-bit count of
  milliseconds can represent, and an operator reads the log
- **THEN** each record's time reference is greater than that of the record before it,
  with no discontinuity at the point where such a counter would have returned to zero

### Requirement: The log occupies a bounded amount of the card

The log SHALL occupy no more than a fixed maximum of the card however long the firmware
runs, by reusing a fixed number of files and replacing the oldest data rather than
adding files. The position within that set SHALL survive a power cycle, so a reboot
resumes rather than overwriting from the beginning.

A card that fills is a card that stops recording, and the firmware has no way to report
that from orbit or to free space on its own.

#### Scenario: Writing continues past the capacity of the whole set

- **WHEN** the firmware writes more data than the configured set of files can hold
- **THEN** the number of log files does not grow, and the oldest data is the data
  replaced

#### Scenario: The board is power-cycled mid-file

- **WHEN** the board loses power partway through the set and is started again
- **THEN** writing resumes within the set rather than at its beginning, and the data
  already recorded elsewhere in the set is retained
