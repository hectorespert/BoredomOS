## Context

`SdData::writeLogIndex()` runs on every rotation: `SD.open("index.bin", FILE_WRITE)`,
`seek(0)`, write a 4-byte `int`, `flush()`, `close()`. In this SD library `FILE_WRITE` is
`O_READ | O_WRITE | O_CREAT | O_APPEND`, and `O_APPEND` moves the offset to the end before
every write (`SdFile.cpp`), so the seek is lost. `readLogIndex()` reads the first four bytes.
See `proposal.md` for the consequences.

## Goals / Non-Goals

**Goals:** the index written by the last rotation is the one the next `begin()` reads.

**Non-Goals:** moving the ring's position into the log data (its own `TODO.md` entry,
*Put the ring's position in the log data instead of `index.bin`*); making the index update
atomic against power loss, which it was not before either.

## Decisions

### Open with `O_WRITE | O_CREAT`, keep the `seek(0)`

No `O_APPEND`, so the write lands at offset 0 and replaces the four bytes in place: one block
written, as before, and no allocation change in the FAT.

*Alternatives rejected:* `O_TRUNC` would also drop the stale bytes old cards carry after offset
4, but truncating frees the file's cluster and the write reallocates it, so a power cut between
the two leaves an empty `index.bin` and the ring restarts at file 0. `SD.remove()` before
creating has the same window. The stale bytes past offset 4 are never read, so leaving them is
harmless.

### No recovery for cards already affected

The first boot after the fix reads whatever stale value the card holds and repeats the fault
once. Recovering would mean guessing the newest file from sizes or from the `TIME` records, which
is a heuristic in the one place that should be exact, for a situation that occurs once per card.
Accepted and stated in the proposal instead.

### The Unity case restarts the object without cleaning the card

It rotates twice so `index.bin` is written twice — the case the current suite never produces,
because `cleanSdFiles()` removes it before every case — records the files' sizes, calls `end()`
and `begin()` as a reboot would, writes once more, and asserts that the file being written grew,
that the other files kept their sizes, and that `index.bin` is 4 bytes long (the last proves the
overwrite on a clean card; the case starts from a card with no `index.bin`).

### Ownership

Unchanged: `lib/SdData` via `src/sdwrite.cpp` owns the card.

## Risks / Trade-offs

- [One more reboot's worth of log lost on affected cards] → Stated; not worked around.
- [The flight-firmware scenario is only visible on the card] → Covered by the Unity case on the
  real card and library; the end-to-end check waits for the card to be readable (pulled, or
  downloaded once `download-the-flight-log` lands).
