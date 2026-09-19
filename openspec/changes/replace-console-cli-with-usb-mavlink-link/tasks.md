# Tasks

Steps marked **[board]** need the assembled board attached. Steps marked
**[hands]** additionally need someone physically at it — pressing RESET, attaching
an adapter, or stalling a host by hand. Nothing in CI can close either kind:
CI runs `pio run` on the environments, the grep that guards static creation, the
grep that forbids `pvPortMalloc` in the link and protocol files, and `pio check`.

Read `design.md`'s Decisions before starting; several tasks below are the direct
consequence of one and the numbering references them.

## 1. Guards first, so nothing silently stops checking

- [x] 1.1 Point `.github/workflows/main.yml:58`'s `pvPortMalloc` grep at
  `src/link.cpp` instead of `src/serial.cpp`, in the same commit as the rename in
  2.2 — verify by running the grep command locally and confirming it still names a
  file that exists (Decision 11).
- [x] 1.2 Remove `-e bench` from `.github/workflows/main.yml:66` and delete the
  `[env:bench]` section and the `CLI_SERIAL` override from `platformio.ini` —
  verify `pio run` succeeds and lists exactly `uno_r4_minima` and `libs`.
- [x] 1.3 Raise `-D MAVLINK_COMM_NUM_BUFFERS=1` to `=2` in `platformio.ini` and
  correct the comment at `platformio.ini:76-84`, which estimates the cost at
  ~315 B — verify with `nm -S .pio/build/uno_r4_minima/firmware.elf | grep
  m_mavlink` that `m_mavlink_buffer` is now 582 B and record the true delta
  (Decision 9).

## 2. The port abstraction

- [x] 2.1 Add `include/LinkPort.h` with `InboundMsg` and `LinkPort` as Decision 3
  defines them, including the `availableForWrite` pointer — verify it compiles
  standalone and that `sizeof(LinkPort)` printed at build time matches the RAM
  table's assumption.
- [x] 2.2 Rename `src/serial.cpp` to `src/link.cpp` with `git mv` so history
  follows, and add the two per-port wrapper functions naming `Serial1` and
  `Serial` concretely — verify `pio run` succeeds and that no `HardwareSerial&` or
  `Stream&` appears in the file (Decision 3's landmine).
- [x] 2.3 Rewrite the reader as one body parameterised by `LinkPort*`, parsing into
  `port.rx` in place and posting `&port.rx`, with the 128-byte drain cap of
  Decision 8 — verify no local `mavlink_message_t` or `InboundMsg` exists in the
  function (Decision 7: a 291 B local overflows the 384 B stack).
- [x] 2.4 Rewrite the writer as one body parameterised by `LinkPort*`, draining
  `port.writeQueue` and calling `port.availableForWrite()` before `port.write()`,
  dropping the frame when there is no room — verify the drop path is taken, not a
  retry loop (Decision 6; this is the watchdog-reset hazard).

## 3. Protocol, per channel

- [x] 3.1 Change `mavlinkPack()` to take a channel and convert all seven
  `mavlink_msg_*_pack()` calls in `src/mavlink.cpp` to `mavlink_msg_*_pack_chan()`
  — verify by grepping that no bare `_pack(` call remains in the file (Decision 5).
- [x] 3.2 Make the dispatch read `chan` off the queued `InboundMsg` and post every
  reply to that port's write queue — verify by inspection that no reply path
  reaches a queue chosen by anything other than the inbound item's `chan`.
- [x] 3.3 Make the housekeeping armed state, interval and round-robin cursor
  per-port arrays — verify the `kName*` table and the schedule table still have one
  entry per task after the renames in 4.3.

## 4. Composition root

- [x] 4.1 Declare the two `LinkPort` instances, `linkReadQueue` (8 ×
  `sizeof(InboundMsg)`), `uartWriteQueue` and `usbWriteQueue` (5 × `sizeof(LinkMsg)`)
  in `src/main.cpp` with static storage — verify `pio run` links and the
  `ram_budget.py` line moves by roughly the amount `proposal.md` predicts.
- [x] 4.2 Create the four link tasks with the names, stacks and priorities of
  Decision 2, passing `&linkPorts[n]` through `pvParameters` — verify each handle
  is `configASSERT`ed and that this is the first `xTaskCreateStatic` in the file
  whose parameter argument is not `NULL` (Decision 4).
- [x] 4.3 Rename `SerialRead`/`SerialWrite` to `UartRead`/`UartWrite` in all three
  places a name lives — `src/main.cpp`, `src/mavlink.cpp`'s `kName*` table and
  `include/Data.h` — verify all four names are ≤10 characters so none truncates in
  `NAMED_VALUE_INT` (Decision 10).

## 5. Remove the CLI

- [x] 5.1 Delete `src/cli.cpp` and `include/Cli.h`, and remove `TaskCli`'s storage,
  declaration and creation from `src/main.cpp` — verify `pio run` succeeds and no
  reference to `CLI_SERIAL` remains anywhere in `src/` or `include/`.
- [x] 5.2 Remove the `Cli` entry from `src/mavlink.cpp`'s housekeeping name table —
  verify the table length matches the number of tasks `src/main.cpp` can create.

## 6. Log schema and heap

- [x] 6.1 Extend `include/Data.h`'s `Tasks` from five fields to seven
  (`uartRead`, `uartWrite`, `usbRead`, `usbWrite`, plus the unchanged three) and
  fill them in `src/logger.cpp` and dump them in `src/sdwrite.cpp`. **Measured
  at compile time, not assumed**: `sizeof(Tasks)` is 28 B and `sizeof(Data)` is
  **44 B** (was 36 B with five fields). This task originally said to verify 64 B,
  which was derived from `ARCHITECTURE.md`'s 56 B — see 6.2.
- [x] 6.2 Re-derive `sdWriteQueue`'s heap backing against the new item size:
  6 blocks × 44 = **264 B** against `configTOTAL_HEAP_SIZE` 0x200 = 512 B,
  leaving 248 B. **This found a pre-existing error**: `ARCHITECTURE.md` §4's
  table said `6 x 56` = 336 B, but `sizeof(Data)` was 36 B before this change,
  so the real backing was 216 B and the documented figure had never been right.
  The change therefore uses *less* of the heap than the doc claimed, not more.
  §4's table is corrected to the measured values.

## 7. Documentation, in the same commit

- [x] 7.1 Redraw `ARCHITECTURE.md` §2's system diagram with two MAVLink endpoints
  and no CLI task — verify no `CONSOLE` or `CLI` node remains.
- [x] 7.2 Correct §3's `pvParameters` rule, the task table, and the four-edits
  recipe; §4's queue table; §5.1's single-parser claim, the port-readiness sentence
  and the `_pack_chan` ownership note; §6's console row; and §8's CLI command table
  — verify by re-reading each section against the built firmware, not against this
  list.
- [x] 7.3 Reword §5.2's "the old seven-field shape", which stops being a unique
  description once the new shape also has seven fields — verify the replacement
  distinguishes the shapes by field name.
- [x] 7.4 Update `CLAUDE.md`'s console paragraph and the `pio device monitor` line,
  and `README.md`'s hardware table, quick-start and "Talking to it" section — note
  `README.md` was already stale about the CLI before this change and gets corrected
  here rather than left wrong in a new way.
- [x] 7.5 Delete the `TODO.md` entry *Remove the console CLI and give USB a
  symmetric secondary MAVLink link* and repoint `platformio.ini`'s comment, which
  cited it by title, at this change id. **Done when this change was proposed**, not
  during implementation: `CLAUDE.md` requires the entry to go in the same commit
  that creates the change, so leaving it for the apply phase would have described
  the same work twice in the meantime. The comment's ~315 B estimate was corrected
  to the measured 363 B at the same time.

## 8. What CI can close

- [x] 8.1 `pio run` on both remaining environments — both link clean under
  `-Wall -Wextra`. **committed 29468 B of 32768 (89.9 %), headroom 3300 B** against
  the 1024 B floor: +2132 B, about 500 B under the estimate. `proposal.md`'s table
  is corrected with the measured figures.
- [x] 8.2 `pio check` — **6 LOW, 0 MEDIUM, 0 HIGH, PASSED**, down from the 13 LOW
  baseline `add-degraded-mode` task 9.5 recorded. No new finding *type*: the four
  in `src/` are the pre-existing `cstyleCast` and `unusedLabel` hits in
  `src/logger.cpp` plus `constVariable` in `src/mavlink.cpp`, and the drop is
  mostly `src/cli.cpp`'s 291 lines leaving the tree. That incidentally clears part
  of 9.5's debt but does not close it — the `src/logger.cpp` cast is still there
  and still undecided.

## 9. Board verification

- [x] 9.1 **[board]** Flash the flight build and connect over USB with no adapter
  attached. **Passed**, 13 s capture on `/dev/cu.usbmodem2101`: `HEARTBEAT`
  1.00 Hz (14), `SYSTEM_TIME` 1.00 Hz (13), `BATTERY_STATUS` 0.50 Hz (6), zero
  `BAD_DATA`. Identity `sys=1 comp=1 type=9 autopilot=0`, `system_status=4`
  (`MAV_STATE_ACTIVE`, normal configuration). The change's main claim holds.
- [ ] 9.2 **[board] [hands]** Attach a USB-TTL adapter to D0/D1 and connect a
  second MAVProxy — verify both ports carry the full telemetry set simultaneously
  and that each shows one vehicle, system 1, `MAV_COMP_ID_AUTOPILOT1`,
  `MAV_TYPE_ROCKET`.
- [ ] 9.3 **[board] [hands]** With both ports attached, read the `seq` field of
  frames on each — verify each advances by one per frame with no gaps attributable
  to the other port's traffic (Decision 5; this is what a missed `_pack_chan`
  would break silently).

  **Partial evidence already in hand, from 9.1's USB-only capture.** The USB
  port's `HEARTBEAT` sequence advanced in steps alternating 2 and 3, never more.
  Each port's schedule emits 2.5 frames a second (`HEARTBEAT` and `SYSTEM_TIME`
  at 1 Hz, `BATTERY_STATUS` at 0.5 Hz), so 2-3 is exactly one port's own
  traffic — and the UART writer *was* running and packing all the while, into an
  unattached port. A shared `current_tx_seq` would have made the step about 5.
  This is strong evidence `_pack_chan` took, but it is not the whole check: it
  reads one stream and infers the other. Left unticked until both are read.
- [ ] 9.4 **[board] [hands]** Send `MAV_CMD_SET_MESSAGE_INTERVAL` for message id
  252 on one port only — verify `NAMED_VALUE_INT` appears on that port and not on
  the other, and that the cycle covers all seven tasks plus the two heap values.
- [ ] 9.5 **[board] [hands]** Open the USB port with a host that does not drain it
  (for example `cat > /dev/null` stopped with SIGSTOP, or a terminal left
  unread) while MAVProxy watches the UART — verify the board does **not** reset,
  the UART keeps its cadences, and the SD log keeps writing at 1 Hz. This is
  Decision 6's hazard and the most important row in this list; no script observes
  it.
- [x] 9.6 **[board]** All seven marks read from the housekeeping stream after a
  300 s soak, armed with `MAV_CMD_SET_MESSAGE_INTERVAL` (`COMMAND_ACK`/ACCEPTED)
  and disarmed after. The cycle delivered **9 distinct values** — the two heap
  figures plus all seven tasks — with every name intact and none truncated,
  which also confirms Decision 10's naming. First pass found two margins out of
  band, so both were resized and re-measured as this task allows:

  | task | size | free | margin |
  |---|---|---|---|
  | `UartRead` | 96 w | 30 | 31 % |
  | `UartWrite` | 384 w | 145 | 38 % |
  | `UsbRead` | 96 → **128 w** | 27 → **59** | 28 % → **46 %** |
  | `UsbWrite` | 384 w | 146 | 38 % |
  | `Mavlink` | 384 w | 155 | 40 % |
  | `Logger` | 96 → **160 w** | 4 → **68** | 4 % → **42 %** |
  | `SdWrite` | 256 w | 53 | 21 % |

  `UsbRead` needed 128 despite running the identical body to `UartRead`, because
  `_SerialUSB::available()`/`read()` reach TinyUSB through deeper call frames.
  `UartRead` at 31 % is the thinnest link task and below the 35-46 band recorded
  before this change, but that band came from short runs rather than a soak, and
  31 % is inside the fleet's range — left at 96 and recorded rather than grown.
  Heap: `HeapFree` 504, `HeapMin` 448 of 512, so a 64 B observed peak.
- [x] 9.7 **[board]** **It had moved, and this change moved it.** `TaskLogger`
  measured **4 of 96 words free**, down from the 6 `TODO.md` records.
  `TaskLogger` builds a `Data` on its stack and `sizeof(Data)` grew 36 → 44 B
  here, which is exactly the two words lost. 4 words is 16 bytes from a silent
  overflow, so the stack was grown to 160 w and re-measured at 68 free (42 %),
  level with `UartRead`. This restores what the change took; it does **not**
  close *[`TaskLogger`'s stack margin is critically tight]*, which asks the
  separate question of why a 1 Hz sampling loop needs 92 words at all.

- [x] 9.8 **[board] — not in the original plan.** `proposal.md`'s `## Why` rests
  on a claim nothing else exercised: that a board needing
  `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` can now be commanded over USB against the
  flight build, with no adapter and no reflash. Demonstrated end to end, and it
  was needed for real — four reflashes this session had taken `cumulative` to
  6 of the 10 that latch the reduced configuration, the hazard
  [TODO.md](../../../TODO.md) already records.

  ```
  BEFORE  custom_mode=0x06000603  phase=scheduler  consecutive=0/3  cumulative=6/10
          -> MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN over /dev/cu.usbmodem2101
          -> COMMAND_ACK command=246 result=0 (ACCEPTED)
  AFTER   custom_mode=0x01000003  phase=start      consecutive=0/3  cumulative=1/10
          -> HEARTBEAT 8, SYSTEM_TIME 9, BATTERY_STATUS 4 in the next 8 s,
             system_status=4 (normal configuration)
  ```

  `Recovery::reinitialise()` cleared both counters and the snapshot; the
  commanded reboot then counted itself, leaving `cumulative` at 1 rather than 0.
  Whether a deliberate reset should be excluded from the cumulative count too is
  `add-degraded-mode`'s question, not this change's — recorded, not acted on.

  This also incidentally confirms what `ARCHITECTURE.md` §3 warns must stay true:
  the 5-minute stability window still fires after the schedule was split per
  port (`consecutive` read 0 after the 300 s soak in 9.6), which only holds
  because `HEARTBEAT`'s entry is unconditional on both ports.

## 10. HIL suite

- [x] 10.1 Delete `test/test_hil/check_cli.py` — verify `run.py --list` no longer
  offers its two cases and the suite count drops accordingly.
- [x] 10.2 Invert `check_silence.py` so it asserts USB **does** carry MAVLink —
  verify it fails against a pre-change firmware and passes against this one.
- [x] 10.3 Make the suite target USB by default with an override selecting D0/D1.
  **Deviation from the task as written**: no `HIL_PORT=uart` was added, because
  `hil.py`'s `find_port()` already prefers the board's own Arduino CDC device and
  already accepts `HIL_PORT` as a device path. USB carrying MAVLink is all that
  was missing, so the default now works with no new mechanism; `HIL_PORT` points
  at an adapter, and `check_dual_link.py` takes `HIL_UART_PORT` for the cases that
  need both ports at once. Only the stale docstring claiming the CDC port "carries
  nothing" was changed. **Not yet verified end to end** — needs 9.1.
- [x] 10.4 Add the four automatable per-port cases named in `proposal.md` — both
  ports emit the same cadences, a reply leaves by the port it arrived on, sequence
  numbering is per port, and arming is per port — with the `NoLinkError` self-skip
  when the second port is unreachable. Written as `check_dual_link.py`.
  **Written, not run**: every case needs a USB-TTL adapter on D0/D1 as well as the
  USB cable, so all four self-skip until 9.2's hardware is present.
- [x] 10.6 **Not in the original plan, found by running the suite.**
  `check_housekeeping.py` hardcoded the expected `NAMED_VALUE_INT` names and
  failed with *"unexpected name(s): ['UartRead', 'UartWrite', 'UsbRead',
  'UsbWrite']"* after 4.3's rename. `NORMAL_NAMES` and `REDUCED_NAMES` updated:
  the normal cycle is 9 values rather than 8, the reduced one 7 rather than 6,
  and no name is truncated any more. This was in scope — the rename caused it —
  but the task list did not anticipate it.
- [x] 10.5 Record 9.5 as a manual `[board] [hands]` step in
  `test/test_hil/README.md` alongside the existing manual-reset one — verify it is
  written down rather than left as a row nothing can run and nothing describes.
