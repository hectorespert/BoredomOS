## MODIFIED Requirements

### Requirement: The log occupies a bounded amount of the card

The log SHALL occupy no more than a fixed maximum of the card however long the firmware
runs, by reusing a fixed number of files and replacing the oldest data rather than
adding files. The position within that set SHALL survive a power cycle, so a reboot
resumes rather than overwriting from the beginning. It SHALL survive a power cycle that
interrupts the move from one file to the next, and the loss or damage of whatever the
firmware keeps to remember it, for as long as the log files themselves are intact.

No log file SHALL ever hold records from two passes through the set.

A card that fills is a card that stops recording, and the firmware has no way to report
that from orbit or to free space on its own. A file holding two passes is worse than a
lost one: its head reports when the older pass began, so a reader dates the newer
records wrongly and has nothing in the file to tell them so.

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

#### Scenario: Power is lost while the log moves on to the next file

- **WHEN** the set has been written through at least once, the board loses power after
  filling a file but before it has started the next one, and it is started again
- **THEN** the log continues in a file that holds only records written after the restart,
  and no file on the card holds records from two passes through the set

#### Scenario: The saved position is lost

- **WHEN** the log has written part of a file, and the record of which file that was is
  removed from the card or damaged so that it names a file that is already full, and the
  board is started again
- **THEN** writing resumes in the file that was only partly written, and every other file
  of the set holds what it held before the restart
