## Context

`src/sdwrite.cpp` owns the card through `lib/SdData`. `TaskSdWrite` runs at `PRIORITY_LOWEST`
— the idle task's — on a 256-word stack with 73 words free, and exists only when the card was
mounted at boot. It blocks on `sdWriteQueue` (depth 4, 32 B `SdRecord`s) and writes records
through `SdData::write()`, which syncs every 4 KiB and rotates the ring when a file reaches
1 MiB: close, advance `index.bin`, **delete** the next slot, open it, write the preamble.
`SdData::begin()` reopens the file `index.bin` names in append mode, so one file can hold
several boots, each starting with the same preamble and a `TIME` record.

Every file starts with four 89-byte `FMT` records, then a 16-byte `TIME` record at offset 356.
The SD library allocates each `File`'s `SdFile` with `malloc` from newlib's heap, and all files
share one 512-byte cache block.

`TaskMavlink` dispatches inbound messages and owns no peripheral. Each port's write task
(`TaskLinkWrite`, `PRIORITY_HIGH`) drains a queue of `LinkMsg` (64 B, `static_assert`ed) at
depth 7, a depth derived in `src/main.cpp` from five periodic entries and two replies that can
coincide on one pass. MAVProxy — the reference ground station — asks for a whole log with
`count = 0xFFFFFFFF`, files chunks by `ofs // 90`, re-requests after 0.7 s without data, and
issues up to twenty range requests at once to fill gaps (`mavproxy_log.py`). It picks "latest"
by `time_utc`, not by id.

See `proposal.md` for why.

## Goals / Non-Goals

**Goals:**

- The card keeps exactly one owner, and no new task.
- A download cannot starve the log or the periodic telemetry, by construction rather than by
  tuning a rate.

**Non-Goals:**

- Speed. The UART sets it; on USB whatever `TaskSdWrite` manages at the idle priority is what
  it is, and this change measures it rather than promising a figure.
- Serving anything but the log files (`index.bin` is FTP's business).
- More than one download per port at a time.

## Decisions

### Every card access for the link happens in `TaskSdWrite`

`TaskMavlink` turns `LOG_REQUEST_LIST`, `LOG_REQUEST_DATA` and `LOG_REQUEST_END` into three new
`SdRecord` kinds carrying the port, id, offset and count, and posts them to `sdWriteQueue`.
`TaskSdWrite` answers by posting `LinkMsg`s straight onto that port's write queue. Nothing else
touches the card, and the write tasks still own their ports.

*Alternatives rejected:* reading in `TaskMavlink` or in the write task (a second owner of the
card); packing and writing the frame from `TaskSdWrite` (a second owner of the port).

### `LinkMsg` grows to carry a `LOG_DATA` by value

`LinkMsgKind::LogData { uint32_t ofs; uint16_t id; uint8_t count; uint8_t data[90]; }` and
`LinkMsgKind::LogEntry { uint32_t time_utc, size; uint16_t id, num_logs, last_log_num; }`. The
first makes the union 104 B and `LinkMsg` about 112 B, and the `static_assert` moves with it
deliberately.

*Alternative rejected:* a 90-byte mailbox per port with a hand-back message, which costs a third
of the RAM and gives flow control for free but breaks "every queue carries its items by value"
(`ARCHITECTURE.md` §4) with a three-task hand-off protocol. It was judged the less simple of the
two. **It stays the fallback** if the measured cost below does not fit.

### Pacing: at most two chunks queued per port, and the write queues go to depth 9

Before posting a chunk, `TaskSdWrite` checks `uxQueueMessagesWaiting` on that port's queue and
posts only if it holds at most one item; otherwise it yields a tick and looks again. So no more
than two chunks are ever queued, the wire stays busy (one chunk transmitting, one waiting), and
the seven slots the periodic worst case needs stay free: the depth becomes 7 + 2 = 9, and the
derivation in `src/main.cpp` gains the two chunks. A periodic message waits behind at most two
`LOG_DATA` frames — about 38 ms at 57 600 baud — so the cadence requirement holds.

The polling runs at the idle task's priority, so it only uses time nobody else wants.

### `TaskSdWrite` writes the log first, and reads only when it has nothing to write

Each pass drains `sdWriteQueue` first, then, if a download is active on a port whose queue has
room, reads and posts **one** chunk, then goes back to the queue with a short timeout. A read
never waits on a record behind it. `sdWriteQueue` goes to depth 6 so that requests cannot fill
it with log records waiting.

`TaskMavlink` forwards a request only when `sdWriteQueue` has at least two free slots. MAVProxy's
gap filling can send twenty requests at once; the ones dropped are sent again by MAVProxy 0.7 s
later, and a dropped record would not be. Each accepted request replaces the port's stream
(`{file, offset, end}`), so a burst of range requests serves the last one and MAVProxy asks for
the rest again — slow, but it converges and it never costs a log record.

### Ids are fixed by slot; the stream pins the file

`dataN.BIN` is id `N + 1` for as long as it exists. Listing reads `index.bin`, each file's size,
and the 16 bytes at offset 356; `time_utc` is `0` when that `TIME` record's source is *none*.

*Alternative rejected:* chronological ids (1 = oldest). Every rotation would renumber every file,
and a MAVProxy retry in flight would fetch a different file under the same id.

A stream holds the slot index it was opened on. After every `SdData::write()`, `TaskSdWrite`
compares `SdData`'s current index (a new getter) with each stream's slot; if the ring has just
reused that slot, it closes the read handle and ends the stream with `count = 0` **before** the
next chunk. Since the deletion happens inside `write()`, on this task, no chunk of the new file
can be read in between.

### The active file: a second, read-only handle, ended at the synced size

A stream opens its file with `FILE_READ` — a handle separate from `SdData`'s `FILE_WRITE` one —
and records `size()` at that moment as its end. That size lags the true length by up to one flush
interval (4 KiB), which is exactly what the requirement promises: the rest comes with the next
request. Reads go through the shared cache, so a block written but not yet synced is still read
correctly.

**This is the assumption the spike tests** (tasks 1.x): that this library tolerates a read handle
on a file open for writing, reports a consistent size, and does not corrupt the writer. If it
does not, the list offers only closed files and the active file's id is answered as absent; the
rest of this design is unchanged.

### One read handle at a time, opened per request

At most one `File` for reading is open at once, across both ports: a second port's request waits
until the first stream ends or is replaced. That bounds the newlib allocation to one `SdFile` at a
time, of the same size every time, which does not fragment. It is closed when its stream ends.

### No card, no `TaskSdWrite`

`TaskMavlink` checks `taskSdWriteHandler == NULL` — the gate `logBattery()` already uses — and
answers there: `LOG_ENTRY` with `num_logs = 0`, and `LOG_DATA` with `count = 0`. Nothing is posted
to a queue nobody reads. `LOG_ERASE` gets its own empty `case`, so it does not fall to `default`
and draw a `STATUSTEXT`.

### Ownership

`src/sdwrite.cpp` (via `lib/SdData`) owns the card, including every read. `src/link.cpp` owns
both ports. `src/mavlink.cpp` owns the protocol decisions and touches neither.

## Risks / Trade-offs

- **RAM: about 1.2 KB of 3 KB headroom, estimated.** → Measured first (task 2.1); if what is left
  after stacks is too thin to leave room for anything else, switch to the mailbox alternative
  before writing the rest.
- **`TaskSdWrite`'s stack.** It gains a 112-byte `LinkMsg` local and a read path through the SD
  library whose depth is unknown, on 73 free words. → Its high-water mark is read after a full
  download; growing it is expected and is more RAM.
- **USB speed at the idle priority** may be poor. → Measured and recorded, not promised.
- **Gap filling converges slowly** because each request replaces the stream. → Accepted; a queue
  of ranges would cost RAM for a case that only arises on loss.
- **The rotation-during-download path** is nearly untestable (a rotation needs ~8.6 h of logging,
  or a special build). → Implemented as described and recorded in `tasks.md` as unexercised if it
  stays so.
- **`time_utc` 0 on several files** makes MAVProxy's "latest" ambiguous. → Honest; not fixed here.

## Migration Plan

Nothing is persisted and the card format does not change. Rolling back is reverting the commit.
