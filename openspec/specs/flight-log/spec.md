# flight-log Specification

## Purpose
Defines what the flight log guarantees to whoever eventually reads it. The log is the
only record of how close a 96-word task came to overflowing, it is written to a card
that will be pulled out and read on another machine, and the expected way for it to end
is the power dying mid-write. This capability states what survives that, what a reader
needs no project-specific software for, and what a recorded figure is allowed to mean.

## Requirements

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

#### Scenario: The board restarts after the log has moved on to another file

- **WHEN** the log has filled one or more files and moved on to the next, and the board is
  then restarted, once or several times
- **THEN** after each restart the log continues in the file it was writing before the
  restart, and every other file of the set holds what it held before the restart

### Requirement: The log can be listed from the ground

A `LOG_REQUEST_LIST` received on either port SHALL be answered, on that port, with one
`LOG_ENTRY` for each file of the log that exists on the card and whose id falls within the
requested range. Each file SHALL keep the same id for as long as it exists, whatever else
the log does: the log file stored in slot `N` of the ring SHALL be id `N + 1`. `num_logs`
SHALL be the number of files that exist and `last_log_num` the highest id among them.

`time_utc` SHALL be the wall-clock time recorded at the head of that file, and SHALL be `0`
when the firmware held no wall clock when the file was opened. `size` SHALL be the number of
bytes a download of that file would deliver at that moment.

#### Scenario: A ground station lists the logs

- **WHEN** a ground station sends `LOG_REQUEST_LIST` with `start` 0 and `end` 0xFFFF on a
  board whose card holds a log
- **THEN** it receives, on that port, one `LOG_ENTRY` per log file on the card, each with the
  same `num_logs` and `last_log_num`
- **AND** a file that has been written for more than a few seconds has a non-zero `size`

#### Scenario: Ids do not move between listings

- **WHEN** a ground station lists the logs twice, with the board logging in between
- **THEN** every id present in both listings has the same `time_utc` in both

#### Scenario: A log opened with no wall clock

- **WHEN** the board opened a log file before any wall clock was available to it, and a ground
  station lists the logs
- **THEN** that file's `LOG_ENTRY` has `time_utc` 0

### Requirement: A listed log can be downloaded, including the one being written

A `LOG_REQUEST_DATA` naming a listed id SHALL be answered, on the port it arrived on, with
`LOG_DATA` messages carrying that file's bytes from offset `ofs`, each carrying 90 bytes at an
offset that is a multiple of 90 except the last, until `count` bytes or the end of the file
have been sent, whichever comes first. The end of the file SHALL be signalled by a message
carrying fewer than 90 bytes, or none. Bytes delivered SHALL be the bytes stored on the card at
those offsets.

The file currently being written SHALL be downloadable too. Its end, for one request, SHALL be
the length the card held for it when that request was received: bytes logged after the request
are delivered by a later request, never mixed into this one's end.

A download SHALL deliver bytes of the file its id named and no other. If the firmware has to
reuse that file's slot for new log data while the download is running, the download SHALL end
early rather than continue with the new contents.

A `LOG_REQUEST_DATA` naming an id that does not exist SHALL be answered with a single
`LOG_DATA` carrying no data. A new `LOG_REQUEST_DATA` on a port SHALL replace the download in
progress on that port. A `LOG_REQUEST_END` SHALL stop the download on that port.

#### Scenario: A whole log is downloaded

- **WHEN** a ground station lists the logs, downloads a closed file whole, and the card is then
  pulled and read on another machine
- **THEN** the downloaded bytes are identical to that file on the card
- **AND** a general-purpose flight-log reader parses the downloaded file

#### Scenario: The file being written is downloaded

- **WHEN** a ground station downloads the file the board is writing
- **THEN** the download ends with a message carrying fewer than 90 bytes, and a
  general-purpose flight-log reader parses the downloaded file
- **AND** the downloaded bytes are identical to the start of that file as later read from the
  card

#### Scenario: A range is downloaded

- **WHEN** a ground station requests 900 bytes of a listed file from offset 1800
- **THEN** it receives ten `LOG_DATA` messages at offsets 1800 to 2610 carrying the bytes of
  the file at those offsets

#### Scenario: An id that does not exist

- **WHEN** a ground station requests data for an id that the last listing did not contain
- **THEN** it receives one `LOG_DATA` with `count` 0

#### Scenario: The ground stops a download

- **WHEN** a ground station sends `LOG_REQUEST_END` while a download is running on its port
- **THEN** no further `LOG_DATA` is sent on that port within two seconds

#### Scenario: The file being downloaded is reused

- **WHEN** a download of the oldest file is running and the log fills the file being written
  and reuses the oldest file's slot
- **THEN** the download ends, and every byte it delivered belongs to the file as it was before
  the reuse

### Requirement: Downloading costs neither the log nor the link's cadence

While a download runs on either port, the log SHALL keep recording at its fixed cadence with no
record lost to the download, and each port SHALL keep sending its periodic telemetry at its
declared rates. A download SHALL go no faster than that allows.

#### Scenario: Telemetry during a download

- **WHEN** a ground station downloads a whole log file over the UART
- **THEN** it keeps receiving `HEARTBEAT`, `SYSTEM_TIME` and `SYS_STATUS` at 1 Hz throughout

#### Scenario: The log during a download

- **WHEN** a download of a whole file runs, and the log is afterwards read, either from the
  card pulled out of the board or by downloading the file being written
- **THEN** the log's once-per-second record has no gap during the time the download ran

### Requirement: The ground is told when there is nothing to download

When the board has no log it can read — it booted with no card, or it runs in the reduced
configuration — a `LOG_REQUEST_LIST` SHALL be answered with a `LOG_ENTRY` whose `num_logs` is
0, and a `LOG_REQUEST_DATA` with a single `LOG_DATA` carrying no data, so that a ground station
is told rather than left to retry.

#### Scenario: Listing with no card

- **WHEN** the board booted with no card and a ground station sends `LOG_REQUEST_LIST`
- **THEN** it receives a `LOG_ENTRY` with `num_logs` 0

#### Scenario: Listing in the reduced configuration

- **WHEN** the board runs in the reduced configuration, with its card still in place, and a
  ground station sends `LOG_REQUEST_LIST` and then `LOG_REQUEST_DATA` for id 1
- **THEN** it receives a `LOG_ENTRY` with `num_logs` 0, and one `LOG_DATA` with `count` 0

### Requirement: No command from the ground erases the log

`LOG_ERASE` SHALL have no effect on the log, and SHALL NOT cause any message to be sent. No
other message the firmware accepts SHALL delete, truncate or overwrite a log file.

#### Scenario: A ground station sends LOG_ERASE

- **WHEN** a ground station lists the logs, sends `LOG_ERASE`, and lists them again
- **THEN** the second listing has the same ids, and each id's `size` is no smaller than in the
  first
