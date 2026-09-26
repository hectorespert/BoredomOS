## MODIFIED Requirements

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
