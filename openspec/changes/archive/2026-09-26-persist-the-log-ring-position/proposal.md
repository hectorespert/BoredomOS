## Why

The log's position in its ring does not survive a restart once the ring has rotated.
`SdData::writeLogIndex()` opens `index.bin` with `FILE_WRITE`, which includes `O_APPEND`, so
the `seek(0)` before the write is overridden and every new index is appended.
`readLogIndex()` reads the first four bytes — the first index ever written. After the first
rotation, every boot reopens that same full file, rotates on its first write, and **deletes
the file the previous boot was logging into**. The log keeps only what happened since the last
reset, which is the opposite of what a flight log is for.

It was found by the spike of `download-the-flight-log`, which saw three consecutive boots
reopen the same 1 MiB file, each a preamble longer. It went unnoticed because the Unity suite
deletes `index.bin` before every case, so the index is never written twice in one test, and a
rotation had never happened on a board doing its job until the ring was shrunk to 4 × 1 MiB.

The existing requirement *The log occupies a bounded amount of the card* already says the
position survives a power cycle; the firmware breaks it.

## What Changes

- `writeLogIndex()` opens `index.bin` for writing without `O_APPEND`, so the index is
  overwritten in place at offset 0.
- A Unity case that rotates twice, restarts the object and checks it resumes in the file it
  was writing, with the other files intact — the case the suite was missing.
- The requirement gains a scenario for a restart after the ring has rotated.

No MAVLink surface, task, queue or library changes.

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `flight-log`: *The log occupies a bounded amount of the card* gains a scenario stating that a
  restart after a rotation resumes in the file being written and deletes nothing.

## Impact

- **Code:** `lib/SdData/SdData.cpp` (one call), `test/test_libs/test_main.cpp` (one case).
- **RAM:** none; the build's `RAM budget` is unchanged by a flag.
- **Cards already in use:** their `index.bin` holds a stale first value followed by appended
  ones. The first boot after this fix reads the stale value, so it repeats the fault **once** —
  reopening that file and rotating over the next slot — and writes a correct index from then on.
  `design.md` says why that is not worked around.
- **Tests:** `pio test -e libs` is destructive (it replaces the firmware and deletes the flight
  log on the card) and needs the board. The flight-firmware scenario itself — a real restart
  after a real rotation — is only observable by reading the card, which cannot be pulled at the
  time of writing, or by the download `download-the-flight-log` will add; `tasks.md` says so.
- **Other active changes:** `download-the-flight-log` (not yet implemented) recorded this defect
  in its task 1.3 and cannot finish that task's remount check until this lands. Nothing it
  claims is made false by this change.
