# Proposal

## Why

Rotation persists the new position in `index.bin` *before* it deletes the file that
occupies the next slot. A power cut between the two leaves an index naming a file from the
previous lap, and the next boot opens that file for append: it gains a new preamble and a
record after the old lap's data, then rotates at once. The result is one file holding two
laps, whose head `TIME` — and therefore its `LOG_ENTRY.time_utc` — belongs to the older
one. `ARCHITECTURE.md` states that a file "never holds records from an earlier lap"; this
window is the exception it does not know about.

Separately, `index.bin` is a single point of failure for the ring's position. If it is
missing or unreadable, `readLogIndex()` returns 0 and the board restarts the ring at
`data0.BIN`. If that file is full, the first write rotates and deletes `data1.BIN`, which
after a wrap is not necessarily the oldest data. The file sizes on the card already say
where the ring was, and nothing consults them.

This replaces the backlog entry *Put the ring's position in the log data instead of
`index.bin`*. That entry's own case was "a thin one": it removed the side-car file to guard
against the side-car being lost. Reordering the rotation and validating the index against
the files closes the same failures without a new record type, a boot-time scan of record
contents, or a change to the log format.

## What Changes

- Rotation deletes the next slot's file **before** it persists the new index, so no crash
  point leaves an index naming a file from the previous lap.
- `begin()` validates the persisted index against the card. It trusts the index when the
  slot it names is absent or not yet full. Otherwise — index missing, out of range, or
  naming a full file — it searches the ring, starting after that slot, for the first slot
  that is absent or not full, and rewrites `index.bin` to name it. If every slot is full,
  it moves on from the slot the index names (slot 0 when there is none) exactly as a
  rotation does. `begin()` never appends to a full file. Today it does, and when that file
  is from the previous lap, that append is what mixes two laps.
- `index.bin` keeps its format, name and location. Cards written by earlier firmware are
  read unchanged.
- `ARCHITECTURE.md`'s ring description is corrected. It currently claims no file holds two
  laps, and does not describe the index being validated.

No MAVLink surface changes: no new message, stream, or id; `LOG_ENTRY` ids stay the slot
number plus one.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `flight-log`: *The log occupies a bounded amount of the card* gains two scenarios. Power
  lost during a rotation leaves no file holding records from two laps, and a card whose
  saved position is lost or damaged resumes in the file that was being written.

## Impact

- **Code:** `lib/SdData/SdData.cpp` and `SdData.h` only (`begin()`, `readLogIndex()`, the
  rotation path in `write()`). `src/sdwrite.cpp` is untouched: it keeps calling `begin()`
  once and reading `currentFile()`.
- **RAM:** no task, queue or library is added, and nothing new in `.bss`. `begin()`
  opens up to one more file per slot at boot to read its size. Each open allocates an
  `SdFile` from newlib's heap and frees it on close, as every open already does, and only
  one is held at a time.
- **Boot time:** up to one directory lookup per slot (four by default), once, before
  `TaskSdWrite` starts logging. There is no FAT chain walk: a size comes from the
  directory entry.
- **Verification:** `lib/` only, so the Unity suite on the board is the proof. The
  power-cut-during-rotation scenario cannot be produced by cutting power at a chosen
  instruction. It is exercised by building the card state that cut would leave (a stale
  index, a stale full file, a missing next file) and calling `begin()`. That shows `begin()`
  handles the state; it does not show the state is the only one a real cut can leave. The
  reordering argument is in `design.md`, and no test can close it.
- **Other active changes:** none are open, so none is invalidated.
- **Backlog:** the `TODO.md` entry *Put the ring's position in the log data instead of
  `index.bin`* is deleted in the commit that adds this change. No other entry
  cross-references it.
