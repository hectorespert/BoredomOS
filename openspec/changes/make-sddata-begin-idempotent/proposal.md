## Why

`SdData::begin()` opens the log file only when it does not already hold one, and there
is no `end()` — nothing ever closes `_dataFile`. A second `begin()` is therefore a
silent no-op that keeps the first file, even when the ring index has moved and a
different file is what should be open.

In flight this is latent: `begin()` runs exactly once, from `TaskSdWrite`, and nothing
restarts the logger. It stops being latent the moment anything does — a card remount, an
error-recovery path, or the failure handling that *SD logging failure is silent* would
add.

It is not latent in the tests. `setUp()` calls `cleanSdFiles()` and then `begin()`;
`tearDown()` calls `cleanSdFiles()` again, which `SD.remove()`s a file the object still
holds open. From the second case onward `begin()` sees a truthy `_dataFile` and does not
reopen, so the remaining cases write through a handle to a deleted file. The suite passes
because it asserts on `SD.exists()`, never on the bytes. The same defect is why the
`onOpen` callback's first-open path — the one inside `begin()` — is covered nowhere:
`setUp()` has already called `begin()` before any test body runs, and a second call
proves nothing.

Why now: the two pieces of work queued behind this one, the ring's file size and the
write/flush policy, both rewrite the open and close path. Doing this first means they
build on a coherent lifecycle rather than preserving an incoherent one.

## What Changes

- `SdData::begin()` becomes idempotent: it closes whatever file it holds, reads the
  index, opens the file that index names, and notifies. Opening always opens.
- A new `SdData::end()` closes the open file, so a caller can put the object back to a
  state where the card may be modified underneath it.
- The Unity suite's `tearDown()` calls `end()` before `cleanSdFiles()`, so the suite
  stops removing a file the object holds open.
- A new Unity case covers `onOpen` firing on an open performed by `begin()`, which is
  reachable for the first time once `begin()` reopens.
- Not breaking. `begin()` keeps its signature, and the single flight call site — with
  nothing open — behaves exactly as it does today.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

None. This change sets `skip_specs: true`.

The behaviour is already specified. `flight-log`'s *Each log file is readable on its own*
already requires every log file to carry the definition of each record type it contains,
and requires a file that is reopened and appended to to carry those definitions again
from the point of reopening. That is exactly what the `onOpen` callback exists to do.
This change makes the implementation satisfy that requirement in a case nobody had
exercised, and adds the test that demonstrates it. What the log guarantees to its reader
does not change.

The proposal instruction is explicit that a requirement should not be invented to satisfy
validation, and every candidate here is already covered:

- "the definitions are written on every open" — covered by the requirement above,
  including the first open.
- "the position within the ring survives a power cycle" — covered by *The log occupies a
  bounded amount of the card*, and unaffected by this change.
- "the test suite does not corrupt the card" — not a property of the product.

## Impact

**Code.** `lib/SdData/SdData.h` and `lib/SdData/SdData.cpp` (the lifecycle);
`test/test_libs/test_main.cpp` (`tearDown()` plus one new case). No change to
`src/sdwrite.cpp`: it registers the callback and calls `begin()` once, and both keep
working unchanged.

**RAM.** No task, no queue, no library. Cost is zero bytes of RAM: no new member, no new
static, and `end()` is flash only. Headroom read from a build run while writing this,
not quoted: **3184 B** committed against 32768, floor `custom_ram_min_headroom` 1024.

**MAVLink surface.** Unchanged. No new message id, no new stream rate, same identity
triple.

**Other active changes.** None. `openspec/changes/` holds no other change, so there are
no competing figures or claimed build flags to invalidate.

**Verification needs the assembled board.** `pio test -e libs` is the destructive suite:
it replaces the flight firmware with the Unity binary and deletes `data*.BIN` and
`index.bin` from the card on every case, so it must be asked for and followed by
`pio run -t upload`. Confirming that the preamble actually reached the file — as opposed
to the callback merely having been invoked — needs the card pulled and read on another
machine. A green `pio run` proves nothing here: `src/` is not even in the `libs` binary.

**Carried, not fixed.** `cleanSdFiles()` still has no way to refuse to remove a file the
object holds; after this change the ordering in `tearDown()` is what prevents it, not a
guard. That was considered and left out — see `design.md`.
