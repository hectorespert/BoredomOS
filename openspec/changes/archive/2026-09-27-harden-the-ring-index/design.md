# Design

## Context

See `proposal.md` for why. The ring lives entirely in `lib/SdData`. `src/sdwrite.cpp`
owns the card and is the only caller: `TaskSdWrite` calls `begin()` once, on its own
320-word stack, then calls `write()` until power goes. Rotation happens inside `write()`,
on the same task. Nothing else opens `index.bin` or a `data<i>.BIN` file for writing. The
log download also runs in `TaskSdWrite`, reads files, and learns about rotation through
`currentFile()`.

A slot is **full** when its file's size is at least the per-file limit. That is the same
comparison `write()` already uses to decide to rotate, so `begin()` and `write()` can
never disagree about it.

Rotation today, and the state each power cut leaves behind:

```
  close(i) --> idx=i+1 --> write index.bin --> remove(i+1) --> open(i+1) --> preamble
           ^                               ^               ^              ^
           A                               B               C              D
```

| Cut | Index says | Slot `i` | Slot `i+1` | Next boot today |
|---|---|---|---|---|
| A | `i` | full | old lap, full | appends to `i` past its limit, rotates: harmless |
| B | `i+1` | full | **old lap, full** | appends preamble + record to the old lap, then rotates: **mixed laps** |
| C | `i+1` | full | absent | creates `i+1`: correct |
| D | `i+1` | full | empty, or preamble only | appends a second preamble: harmless, and already covered by *Each log file is readable on its own* |

## Goals / Non-Goals

**Goals:**
- No crash point in rotation leaves a state from which the next boot writes into a file
  from an earlier lap.
- A missing, out-of-range or stale `index.bin` is repaired from the file sizes instead of
  sending the ring back to slot 0.

**Non-Goals:**
- Ordering logs across laps for the ground. `LOG_ENTRY` ids stay slot + 1. A lap counter
  is the only thing that would give that, and it was deliberately not chosen (see below).
- Detecting an `index.bin` that is damaged into a *different valid, not-full* slot. See
  Risks.
- Recovering a log file that is itself damaged. That is the job of *Power loss costs a
  bounded and declared amount of log*.

## Decisions

### 1. Delete the next file before persisting the index

The new rotation order is `close(i)`, `remove(i+1)`, write the index as `i+1`, then
`open(i+1)` and write the preamble. The cut points are then:

| Cut | Index says | Slot `i` | Slot `i+1` |
|---|---|---|---|
| after close | `i` | full | old lap, full |
| after remove | `i` | full | absent |
| after index | `i+1` | full | absent |
| after open | `i+1` | full | empty, or preamble only |

No row pairs an index of `i+1` with an old-lap file in `i+1`. Decision 2 handles the first
two rows, where the index still names a full `i`. On its own, this reordering would leave
the first row appending to a full file, as cut A does today. That doesn't mix laps, but
decision 2 removes it anyway.

*Alternative considered:* write the index last, after the open. That moves the
problem instead of removing it: a cut after the open but before the index leaves a fresh
`i+1` and an index still naming the full `i`. Decision 2 would recover from that too, but
then the ordering proves nothing on its own. Deleting before persisting keeps the one
invariant simple: **the index never names a file from a previous lap.**

### 2. `begin()` validates the index against the card

```
  i = index.bin if present and in range, else 0
  if slot i is absent or not full:            resume in i
  else, for s = i+1, i+2, ... (mod N, N-1 steps):
       if slot s is absent or not full:       resume in s, rewrite index.bin
  if every slot is full:                      advance from i exactly as rotation does
```

- **The index stays the primary source.** When it names an absent or partial slot, it
  wins without a second look. That is the common path, and it costs one size lookup.
- **The search starts after `i`, not at 0.** On the first lap the slots after the current
  one don't exist yet, so the first slot after a stale `i` that is absent or not full is
  the one that follows it. After a wrap there is exactly one such slot, whichever way the
  ring is walked.
- **When everything is full, move on rather than append.** This is the only way `begin()`
  opens a file it did not choose to resume. It goes through the same close / remove /
  persist / open / preamble sequence as `write()`, so `onOpen` fires exactly once, as it
  does for any other open. This covers the "after close" row above without mixing, and
  makes `begin()` never append to a full file.
- **`index.bin` is rewritten only when it was wrong.** A boot that agrees with the index
  writes nothing, which keeps the common path's write count where it is today.

*Alternative considered:* drop `index.bin` and derive the position from sizes
alone. After a wrap that works whenever exactly one slot is partial. But in the "after
close" state every slot is full, sizes alone cannot say which is newest, and the index is
the only tiebreaker. Keeping the index costs nothing and resolves that case.

*Alternative considered:* the backlog entry's lap counter, as a `FMT`-described record
in each file's preamble, with boot picking the file with the highest counter. It gives
the ground a real order across laps, which this design does not. It costs a record type,
reading record contents (not just directory entries) from every slot at boot, a counter
that has to survive in RAM, and a spec change to the log format. The deciding question
was whether the ground needs to know which log is newest after a wrap. It does not, for
now. If that changes, the counter can be added on top of this. Nothing here stands in its
way.

*Alternative considered:* use the FAT modification times. The firmware registers no
`dateTimeCallback`, so every file carries the same default stamp.

### 3. Ownership stays where it is

`lib/SdData` owns `index.bin` and the `data<i>.BIN` files, as it does today. The size
lookups in `begin()` are new opens of files this library already owns. `src/sdwrite.cpp`'s
own `slotSize()` for the download path is not reused: a library reaching into its caller
would invert the dependency, and the duplication is a few lines.

## Risks / Trade-offs

- **An `index.bin` damaged into a different valid slot that is absent or not full** is
  trusted. On the first lap that can name an absent slot ahead of the real one. Logging
  resumes there, the real partial file is left behind, and the slots in between stay
  absent until the ring wraps. No data is lost, and the order on the card is off by the
  skipped slots. → Accepted: telling that apart needs a lap counter (see decision 2).
- **Everything full at boot moves on from `i`**, deleting slot `i+1`. If the index is
  also missing, `i` is 0 and slot 1 may not be the oldest. → Accepted: it needs a cut in
  the few milliseconds between `close(i)` and `remove(i+1)` *and* a lost index at once, and
  it costs one file, which is the bound the requirement already allows for replacing data.
- **Stack of `TaskSdWrite`.** `begin()` now holds a `File` and a loop counter
  in the search. The `File` is small, and only one is open at a time. → The high-water
  mark after a boot is checked (tasks). It is not assumed.
- **Boot heap churn.** Each size lookup allocates and frees one `SdFile` from newlib's
  heap. That heap already absorbs one open per rotation and up to two per download. →
  Nothing to mitigate beyond never holding two open in `begin()`.
- **The crash-point argument is proven by reasoning, not by cutting power.** The Unity
  cases build each row's card state by hand and call `begin()`. That shows the recovery
  from each state. That these are the only states a real cut leaves rests on the ordering
  in decision 1 and on the SD library completing `remove()` and the index write as single
  directory updates. → Stated in the proposal's Impact. It is not claimed as tested.

## Migration Plan

None needed. A card written by earlier firmware has an `index.bin` in the same format,
and is read as it is. The only state specific to old firmware is cut B: the index names an
old-lap file in `i+1`. On a wrapped ring every slot is then full, so decision 2 moves on
from `i+1`. The laps are not mixed, but it deletes slot `i+2` rather than the stale `i+1`,
so one more file is lost than the ideal. That happens once, on a card that crashed at that
exact instant under the old firmware. Rollback is a reflash: the old `begin()` reads the
same `index.bin`.
