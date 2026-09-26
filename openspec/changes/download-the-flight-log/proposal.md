## Why

The flight log can only be read by pulling the SD card out of the board. That is not a way
of reading it with the satellite assembled, and it is no way at all once it flies. The log
is DataFlash `.BIN` since `replace-messagepack-log-with-dataflash`, which ground tools
parse without project-specific code, and the ring is 4 × 1 MiB since
`size-the-log-ring-and-batch-its-flushes`, sized so that one file comes down a 57 600 baud
link in a few minutes. What is missing is the path off the card: the MAVLink log protocol,
which MAVProxy (`module load log`) and QGroundControl (*Analyze → Log Download*) already
drive with no configuration.

## What Changes

- `LOG_REQUEST_LIST` (117) is answered with one `LOG_ENTRY` (118) per file of the ring that
  exists on the card. Each file keeps a fixed id for as long as it exists — `dataN.BIN` is
  id `N + 1` — and its `time_utc` is the wall-clock time in the `TIME` record written at
  the head of the file, or `0` when that record says the clock was never set.
- `LOG_REQUEST_DATA` (119) streams `LOG_DATA` (120) from the requested offset, 90 bytes a
  frame on multiples of 90, up to the requested count or the end of the file, ending with a
  short frame or one of `count = 0`. The file being written is offered too: its end is the
  size the card held when the request arrived, so a download always ends cleanly even while
  the file grows.
- `LOG_REQUEST_END` (122) stops the stream on that port.
- `LOG_ERASE` (121) is received and has **no effect**. It would delete the only record of
  the mission with no confirmation and no reply, and nothing needs it.
- With no card, or in the reduced configuration, the ground is told there are no logs, and
  a data request ends at once with `count = 0`, rather than being met with silence.
- The card is read only by `TaskSdWrite`, which already owns it; the stream is paced by the
  space left in each port's write queue, so periodic telemetry keeps its cadence while a
  download runs.
- **Before any of this is built**, a spike on the board establishes that the SD library
  can read the file it is writing through a second handle without corrupting it. If it
  cannot, the active file is **not** offered — only the three closed ones are — and the rest
  of this change stands unchanged.

Not in scope: MAVLink FTP, which reaches files by path and has its own `TODO.md` entry;
this change settles the card-access design it will reuse. How QGroundControl names the
downloaded file from `MAV_AUTOPILOT_GENERIC` is left open, as
`replace-messagepack-log-with-dataflash` left it.

**MAVLink surface.** New outbound ids 118 `LOG_ENTRY` and 120 `LOG_DATA`; newly handled
inbound 117, 119, 121 (acknowledged by doing nothing) and 122. No new periodic stream and no
change to any existing rate. The identity triple (system `1`, `MAV_COMP_ID_AUTOPILOT1`,
`MAV_TYPE_ROCKET`) does not change.

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `flight-log`: gains requirements that the log can be listed and downloaded over the link,
  including the file being written; that a download costs the log no records and the link no
  cadence; that the ground is told when there is nothing to download; and that no command
  from the ground erases the log.

## Impact

- **RAM — the largest cost of any recent change, and measured before it is committed.** The
  current build reports `committed 29728 B of 32768 (90.7%)`, `headroom 3040 B (minimum
  1024)`. A `LOG_DATA` carries 90 bytes of data and today's `LinkMsg` is 64 B, held there by
  a `static_assert`, so the item grows to about 112 B. `design.md` sets each write queue at
  depth 9 (7 today) so two queued chunks cannot crowd out the worst case the depth was sized
  for. Estimated from those figures, not read from a build: the two write queues go from
  2 × 7 × 64 = 896 B to 2 × 9 × 112 = 2016 B, **+1120 B**; `sdWriteQueue` goes from depth 4
  to 6, +64 B; each queue keeps its one `StaticQueue_t`, unchanged in number. That leaves
  roughly 1850 B of headroom before stacks. Every task that holds a `LinkMsg` local also
  grows by 48 B of stack, and `TaskSdWrite` gains a read path of unknown depth on a stack
  with 73 words free. The first code task measures all of this; a stack that has to grow is
  more RAM on top.
- **Run-time allocation.** Opening the file to read calls `SD.open`, which allocates its
  `SdFile` from newlib's heap (the 8192 B `.heap` section), not FreeRTOS's — the same kind of
  allocation `index.bin` already makes on every boot and rotation. It is not new, but it is
  one more, per download, and `design.md` says how it is bounded.
- **Code.** `include/LinkMsg.h`, `include/SdRecord.h`, `src/mavlink.cpp` (dispatch, answering
  when there is no card), `src/sdwrite.cpp` (list, read, stream, pacing), `lib/SdData` (the
  current file index, and noticing a rotation), `src/main.cpp` (queue depths). The card
  keeps one owner.
- **`ARCHITECTURE.md`.** §4 (queue sizes and the `LinkMsg` size), §6 (`TaskSdWrite` answering
  the link, `TaskMavlink`'s new cases) and the note that nothing allocates at run time.
- **Tests.** New HIL cases for listing, downloading, ending and the no-card answer; the
  downloaded file is checked against the card by `DFReader`, which needs the card pulled.
  **Two scenarios have no practical check**: that a download across a rotation never mixes
  two files (a rotation takes ~8.6 h of logging) and that no record is lost while a download
  runs (that needs the card read afterwards against a timed download). Both are written as
  contract and `tasks.md` says which runs cover them and which do not.
- **Other active changes.** None; `openspec list` is empty.
- **`TODO.md`.** *Download the flight log over the MAVLink log protocol* is deleted in the
  same commit. *Serve the SD card over MAVLink FTP* cites it for the card-access design and
  calls `LOG_ERASE` "the sanctioned way" to destroy the log; both are re-pointed at this
  change.
