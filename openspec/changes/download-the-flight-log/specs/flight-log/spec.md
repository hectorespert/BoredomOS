## ADDED Requirements

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
