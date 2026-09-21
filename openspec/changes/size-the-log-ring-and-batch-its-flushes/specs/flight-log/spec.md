## RENAMED Requirements

- FROM: `### Requirement: A truncated record costs only that record`
- TO: `### Requirement: Power loss costs a bounded and declared amount of log`

## MODIFIED Requirements

### Requirement: Power loss costs a bounded and declared amount of log

A log file whose last record is incomplete SHALL remain parseable up to the truncation,
and a reader SHALL be able to resume at the next complete record following an unreadable
stretch anywhere in the file.

Power loss SHALL cost no more than a bounded, declared quantity of recently written log,
and that bound SHALL be a property of the firmware rather than of how long the board
happened to be running. The bound SHALL be small enough that the figures the log exists
to carry — a task's stack headroom, a battery reading — are not lost for any period long
enough to change a conclusion drawn from them.

Power loss mid-write is the normal ending, not an exceptional one, and a boot that reopens
the same file puts the damaged stretch in the middle rather than at the end. This
requirement used to promise that the incomplete tail was the *only* data lost, which
described a firmware that pushed every record to the card as it was written. That cost
about two sector writes per record, half of them to the same directory sector, and made
the card's filesystem — not the log — the part with a wear limit. What replaces it is a
bound that is declared and checkable rather than one that is as small as possible.

#### Scenario: Power is removed while a record is being written

- **WHEN** power is removed from the board while the log is being written, and the card is
  then read
- **THEN** every record up to the declared bound before the interruption is decoded, and
  what is missing is a contiguous stretch at the end of the file no larger than that bound

#### Scenario: The amount at risk does not grow with uptime

- **WHEN** power is removed from a board that has been logging for minutes and, on another
  occasion, from one that has been logging for days
- **THEN** the amount of log missing from the end of the file is of the same order in both
  cases

#### Scenario: A damaged stretch sits in the middle of a file

- **WHEN** a file containing an unreadable stretch followed by further complete records is
  parsed
- **THEN** the records after the damaged stretch are decoded, rather than the remainder of
  the file being lost

## ADDED Requirements

### Requirement: Maintaining the log never resets the board

No operation the firmware performs to maintain the log — including reusing a file, freeing
the space its previous contents occupied, and opening its replacement — SHALL take long
enough to trigger the watchdog, and none SHALL be allowed to make the board unreachable
while it runs.

The log is the lowest-priority thing this firmware does and nothing waits on it, so it has
no claim on the board's availability. Reusing a file is the one step in that path whose
duration is set by the card and the filesystem rather than by the firmware, and it grows
with the size of the file being reused; a watchdog reset there would cost the log's tail,
restart the mission clock, and count towards the fault threshold that selects the reduced
configuration — a self-inflicted degradation caused by housekeeping.

#### Scenario: The log reuses a file while a ground station is watching

- **WHEN** the firmware has been logging long enough to have reused every file of the set
  at least once, and a ground station has been receiving telemetry throughout
- **THEN** the reset reason the board reports has not changed, its reported time since
  boot has increased monotonically across each reuse, and no telemetry gap longer than one
  reporting interval was observed

#### Scenario: The fault counters are unchanged by ordinary logging

- **WHEN** a board that has been logging normally, with no hardware fault and no commanded
  reboot, is asked for its accumulated fault counts
- **THEN** those counts are no higher than they were before the logging began
