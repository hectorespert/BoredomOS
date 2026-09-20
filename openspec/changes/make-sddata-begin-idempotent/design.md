## Context

See `proposal.md` — Why for the defect. What matters to the approach is the shape of the
current lifecycle:

```
  begin()   _fileIdx = readLogIndex()
            if (!_dataFile) { open; notifyOpened(); }     <- conditional
  write()   append; if full { close; advance; open; notifyOpened(); }
  (none)    nothing ever closes _dataFile outside rotation
```

Rotation inside `write()` already does the full sequence — close, advance, persist,
delete, open, notify — so the code that `begin()` needs exists a few lines below it.
`begin()` is the odd one out.

Two constraints shape everything here.

**Ownership.** `lib/SdData` is the only owner of `_dataFile` and of the ring, the index
and rotation; `src/sdwrite.cpp` owns the card and the meaning of the bytes, and is the
only caller of `SdData`. `lib/SystemTime` owns the clocks and `lib/Battery` the ADC —
neither is touched. This change adds no second owner of anything: `end()` is a method on
the object that already owns the handle, and the test calls it through the same public
interface `src/sdwrite.cpp` uses. Nothing new reaches the card.

**The test environment cannot see a task.** `test_build_src` is off for `libs`, so `src/`
is not in the Unity binary. Every claim this change makes about `TaskSdWrite` is reasoning
about a call site the suite cannot execute, and must be marked as such.

## Goals / Non-Goals

**Goals:**

- `begin()` means "open the file the index names", unconditionally, whatever the object
  was holding. **Reopen semantics, not idempotence** — every call closes, opens and
  notifies, so two calls append two `FMT` preambles. See `proposal.md` for why that is
  what `flight-log` already requires of a reopened file, and for why the change id
  overstates the property.
- A caller can close the file, so the card can be modified underneath without the object
  holding a handle to something that no longer exists.
- The `onOpen` callback's `begin()` path becomes reachable from the Unity suite, closing
  the coverage gap recorded in *Finish what replace-messagepack-log-with-dataflash left
  open*.

**Non-Goals:**

- Any change to rotation, to the file size, or to when `flush()` is called. Those are the
  next two pieces of work and they are deliberately separate; see the backlog entries
  this change's archiving will leave.
- Error reporting. `begin()` still returns `void` and a failed open is still silent —
  that is *SD logging failure is silent*, which this change does not attempt.
- Making `_fileIdx` recoverable without `index.bin`.

## Decisions

### `begin()` closes and reopens rather than returning early

```cpp
void SdData::begin()
{
    if (_dataFile) {
        _dataFile.close();
    }
    _fileIdx = readLogIndex();
    String logFileName = getLogFileName();
    _dataFile = SD.open(logFileName.c_str(), FILE_WRITE);
    if (!_dataFile) {
        return;
    }
    notifyOpened();
}
```

The `if (!_dataFile)` guard is removed, not inverted. Keeping any conditional preserves
the bug in a narrower form: the object would still have to decide whether the file it
holds is the one the index names, and it has no way to know — `getLogFileName()` derives
the name from `_fileIdx`, which `readLogIndex()` has just overwritten. Closing first
removes the question.

`close()` before `readLogIndex()` and not after: `readLogIndex()` opens `index.bin`,
which evicts the single 512 B `SdVolume` cache block. Closing first means the data file's
own `sync()` happens before that eviction rather than racing it.

**Alternative considered: `begin()` returns early when the file it holds is already the
right one.** Cheaper by one close/open on a redundant call, and rejected: the only caller
that would benefit is a caller that does not exist, and it reintroduces the comparison
that cannot be made correctly.

### `end()` is public even though the firmware never calls it

The firmware has no use for `end()`: `TaskSdWrite` calls `begin()` once and then writes
until power goes. It exists because the alternative is worse — the Unity suite's
`tearDown()` has to stop removing a file the object holds open, and without `end()` its
only options are to reach into `SdData`'s internals or to leave the removal as it is.

This is deliberately a narrow public method and not a general "restart" API. It closes
the handle and nothing else: it does not reset `_fileIdx`, does not clear `_onOpen`, and
does not touch `index.bin`. A `begin()` after an `end()` is the documented way back, and
it is exactly the path the new test exercises.

**Alternative considered: a destructor.** `SdData` is a file-scope object in both
`src/sdwrite.cpp` and the test; its destructor runs at a point neither controls, and
never in the firmware. It would not help the test.

### `cleanSdFiles()` gets no guard; ordering is what protects it

The entry this change comes from offered a third option: have `cleanSdFiles()` refuse to
remove a file `SdData` still holds, so a future reordering of `setUp`/`tearDown` fails
loudly instead of silently. Rejected, for one reason: it requires `SdData` to expose
which file it has open — a `openFileName()` accessor — and that accessor is public API
added for the benefit of a test, describing internal state, with no other caller. The
ownership rule exists to keep the number of things that know about `_dataFile` at one.

The cost of rejecting it is real and is accepted: after this change nothing structurally
prevents a future `tearDown()` from removing an open file again. What prevents it is the
order of two lines and the comment above them.

### The new test asserts the callback fired, not that the file is on the card

The Unity suite cannot verify the preamble's bytes: `cleanSdFiles()` deletes the files it
would have to read, on every case, and the suite has no reader for DataFlash. So the new
case registers a callback that counts its invocations and asserts the count moves on a
`begin()` after an `end()`.

That is a weaker claim than "the preamble reached the file", and the difference is why
this change also carries a `[board]` `[hands]` step to pull the card and look. Asserting
the callback fired proves the wiring; only the card proves the bytes. Recording which of
the two a step establishes is the whole point — this project has already once marked a
callback as verified on the strength of a preamble built in Python.

## Risks / Trade-offs

**`begin()` now closes a file that `src/sdwrite.cpp` may believe is open** → It cannot:
`src/sdwrite.cpp` calls `begin()` once, before the first write, with nothing open. The
close is a no-op on that path. Verified by reading the call site, not by the suite, which
cannot reach it.

**A close/open pair costs two card operations that the flight path did not pay** → It
pays them once, at boot, where `TaskSdWrite` is not yet under any deadline. The watchdog
is refreshed from the idle hook and `setup()` has already completed.

**`end()` leaves the object in a state where `write()` silently does nothing** →
`write()` and `writeRaw()` already return early on a falsy `_dataFile`, which is the same
state a failed open leaves behind, so this adds no new silent-failure mode. It does widen
an existing one, and the existing one is *SD logging failure is silent*.

**The suite's remaining cases still delete `data*.BIN` every time** → Unchanged by this
change and unchanged on purpose. `.mpk` files from before the DataFlash change survive;
`.BIN` files do not, which is why the suite is opt-in.

## Migration Plan

None. No on-disk format changes, no stored state changes meaning, and a card written by
the current firmware is read identically by the new one. Rollback is reverting the commit;
there is nothing to undo on the card.

## Open Questions

None that can be deferred. The two that came up while writing this — what the flush
policy should be, and whether the ring's file size should shrink so rotation actually
happens — both change the approach rather than sitting underneath it, which is why they
are separate changes and not questions parked here.
