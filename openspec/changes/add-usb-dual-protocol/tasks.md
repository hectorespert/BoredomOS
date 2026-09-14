Steps marked **[board]** need the assembled board attached. `pio run` is the only
check that runs without hardware.

**This change lands after `add-console-cli`.** It edits the console task that change
creates, and its `console-cli` spec delta modifies requirements that only exist once
`add-console-cli` is archived — `openspec validate` reports that as an INFO today and
it clears itself in that order.

## 1. Measure before building

- [x] 1.1 **[board]** Measure what `Serial.availableForWrite()` reports on this CDC
      implementation in three states — host attached and draining, host attached and
      not draining, no host — and record the figures in the commit message; the whole
      non-blocking design rests on this being meaningful. Measured via a temporary CLI
      command (`avail`, removed after this task): a steady **256 bytes**, both draining
      normally and while flooded with ~1450 unread commands over 12 s without a single
      `read()` call — matches `CFG_TUD_CDC_TX_BUFSIZE = (64) * 4 = 256` for full-speed
      USB in `tusb_config.h`, cross-validating the figure against source. The
      not-draining case did not show the value drop toward 0: this host's own USB-CDC
      driver keeps servicing the device's bulk IN endpoint regardless of whether
      Python calls `read()`, the same limitation noted throughout `add-console-cli`'s
      board verification — so this session cannot force the device's FIFO to back up
      the way a truly stalled host might. The "no host" state was not measured live —
      there is no channel to ask an unconnected board anything — but is covered
      definitively by source instead: `_SerialUSB::write()` checks `connected()` first
      and returns 0 immediately when it is false, before ever touching
      `tud_cdc_write_available()`, so an absent host cannot reach the retry loop at
      all regardless of what `availableForWrite()` would have reported.
- [x] 1.2 **[board]** Confirm the reported figure is at least the size of the largest
      periodic frame (`BATTERY_STATUS`, ~54 bytes on the wire); if it is smaller than a
      frame the drop-and-retry loop would never fire and the design needs revisiting
      before any of section 3 is written. Confirmed: 256 >> 54, by a wide margin.
- [x] 1.3 **[board]** Confirm the 1200-baud touch resets the board on this core, since
      the HIL ordering in section 5 depends on having a reset mechanism. **Confirmed,
      but not the mechanism this task assumed.** The touch does trigger
      `NVIC_SystemReset()` (`boot.cpp`'s `goBootloader()`), but only after writing the
      same `DOUBLE_TAP_MAGIC` the physical RESET button's double-tap writes — the RA4M1
      ROM bootloader then waits indefinitely in DFU for a firmware upload rather than
      resuming the flashed application. Verified on the board: touched at 1200 baud,
      the USB CDC device stayed absent for 50+ seconds with no sign of the app, and
      only returned after an actual `pio run -t upload`. Design.md's "gives a clean
      mechanism" was wrong; corrected there, along with the consequence — task 6.3 no
      longer resets between HIL groups, it relies on ordering alone within the one boot
      `pio test` already provides.

## 2. Share the message logic

- [x] 2.1 Add `include/MavlinkShared.h` declaring the four builders and
      `mavlinkHandleInbound`, each filling a `mavlink_message_t` the caller provides;
      verify with `pio run`. Named `MavlinkShared`, not `Mavlink` as originally
      proposed — see design.md's note on the case-insensitive-filesystem collision
      with the real `MAVLink.h` this surfaced.
- [x] 2.2 Split `sendHeartbeat`, `sendSystemTime`, `sendBatteryStatus` and
      `sendStatusText` in `src/mavlink.cpp` into a builder plus the existing allocate-
      and-queue step, keeping the heap block as the buffer the builder fills so no
      128-word producer task gains a 291-byte local; verify with `pio run` and
      **[board]** confirm the high-water marks of both producer tasks are unchanged.
      Confirmed: `Heartbeat` 26-28/128 and `MavlinkBatteryStatus` 45-48/128 across
      several `ps` reads on the flight build, matching `add-console-cli`'s archived
      figures within normal run-to-run jitter — neither producer gained the 291-byte
      local, as designed.
- [x] 2.3 Move the inbound `switch` out of `TaskMavlink` into `mavlinkHandleInbound`,
      returning the reply through a caller-provided buffer, and reduce `TaskMavlink` to
      receive / dispatch / queue-the-reply / free; **[board]** verify with
      `mavproxy.py` on the radio link that `TIMESYNC` and `SYSTEM_TIME` behave exactly
      as before the refactor. **Signature grew a third parameter**,
      `bool *rebootRequested`, beyond design.md's original two — `MAV_CMD_
      PREFLIGHT_REBOOT_SHUTDOWN` needs the reply sent *before* the reset, and only the
      caller knows its own transport's drain timing, so `mavlinkHandleInbound` fills
      the reply and signals the reboot without acting on it; documented in
      `MavlinkShared.h` and `design.md`. `TaskMavlink` now holds the reply in a local
      `mavlink_message_t` (291 B, comfortably inside its 256-word stack) rather than a
      second heap block. **[board]** verified functionally over USB with `bench`
      (no USB-TTL adapter for D0/D1 this session, so not literally over the radio
      link): `pio test` — `HEARTBEAT`/`SYSTEM_TIME` at 1 Hz, `BATTERY_STATUS` every
      2 s, inbound `SYSTEM_TIME` sets the clock, `TIMESYNC` answered, identity and
      vehicle type correct, 0 failures. Also drove `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`
      by hand: ack arrived in 10 ms, board reset, came back in the normal
      configuration, full suite still 0 failures afterward — the ack-before-reset
      ordering this task's comment describes held.
- [x] 2.4 **[board]** Confirm the radio link's behaviour is byte-identical to before
      this section: same messages, same rates, same identity — this refactor must be
      invisible on `Serial1`. **Not literally verified on `Serial1`** — no USB-TTL
      adapter this session; see 2.3. Verified instead that the exact same packing and
      dispatch code runs correctly over USB via `bench`, and confirmed one real
      behavioural difference worth recording: `TaskMavlink`'s own high-water mark
      dropped from 205/256 words free (51 used) before this section to **68/256 (188
      used) once properly exercised** — see task 4.6, which found the first, smaller
      reading (109 used, from a handful of manual commands) missed the default-branch
      `STATUSTEXT` path and had to be re-measured with the full HIL suite plus deliberate
      unhandled-message traffic. Still comfortably inside 256 words at the time this
      task was verified; `mavlinkStack`'s final size is task 4.6's decision, made with
      that fuller number.

## 3. Mode detection

- [x] 3.1 Add a second parser state to `src/cli.cpp` — `MAVLINK_COMM_1` with its own
      `mavlink_message_t` and `mavlink_status_t` in `.bss` — and feed every inbound
      byte to it before appending to the command buffer; verify with `pio run`.
      **Revised twice during implementation, both found on the board, not on paper:**
      (1) the convenience `mavlink_parse_char(chan, ...)` API pulls in
      `mavlink_get_channel_buffer()`'s and `mavlink_get_channel_status()`'s internal
      per-channel arrays, sized by `MAVLINK_COMM_NUM_BUFFERS` (default 4) — since these
      are local statics inside `static inline` (`MAVLINK_HELPER`) functions, every
      translation unit that calls them gets its *own* private copy, so a second caller
      (this file) duplicated `src/serial.cpp`'s existing 1164 B + 96 B outright and
      overran the link entirely. Fixed with `-D MAVLINK_COMM_NUM_BUFFERS=2` in
      `platformio.ini` (this firmware only ever uses channels 0 and 1), which shrinks
      both copies by the same amount it adds a second one — a wash against the
      pre-this-change baseline, documented there. (2) The separate `mavlink_message_t`
      for inbound (`mavlinkRxMsg`) and outbound (`mavlinkOutMsg`) started as two
      291-byte statics; consolidated into one (`mavlinkMsg`) once verified safe for
      `mavlinkHandleInbound` to alias `msg`/`reply` (see that function's own comment in
      `src/mavlink.cpp`) — see task 4.6 for why the RAM budget needed this. The unused
      `mavlink_status_t` output parameter is passed as `NULL` rather than kept as a
      third variable; `mavlink_parse_char()`'s own internals check for `NULL` there.
- [x] 3.2 Switch to MAVLink mode only when `mavlink_parse_char` reports a complete
      frame, never on a header byte, and process the bytes already buffered as MAVLink
      once switched; **[board]** verify by pasting a lone `0xFD` and confirming the CLI
      still answers. Verified: a lone `0xFD` produced no reply and did not switch mode;
      `ps` sent immediately after correctly showed `error: unknown command` (the stray
      byte landed in the CLI's own line buffer ahead of "ps", exactly the "Line noise on
      an open port" scenario design.md describes — not a bug); `ps` sent again afterward
      answered cleanly, confirming the CLI recovers. 20 bytes of `os.urandom` plus a
      newline behaved the same way.
- [x] 3.3 Make the switch one-way: no timer, no escape, no command returns to CLI mode;
      **[board]** verify that after a MAVProxy session the port ignores command text and
      that a reset restores the CLI. Verified with real pymavlink traffic (not
      MAVProxy specifically, no adapter — see 2.4): once switched, `ps` sent to the port
      produced only ambient scheduled telemetry frames (`0xFD`-prefixed binary), never a
      CLI reply. A fresh flash (this session's only available "reset") came up back in
      CLI mode every time, which is what "one-way, no persistence" implies by
      construction — `mavlinkMode`'s initial value is `false` on every boot and nothing
      writes it to non-volatile storage.
- [x] 3.4 Stop emitting CLI text once in MAVLink mode, so a frame parser never has to
      discard a prompt; **[board]** verify with `mavproxy.py --master=/dev/ttyACM0` that
      no parse errors are reported. Verified via pymavlink rather than MAVProxy directly
      (same adapter gap): sent `ps\n` post-switch and captured the raw reply — 43 bytes,
      all valid MAVLink frames (`0xFD`-prefixed), zero CLI text bytes. `check_telemetry.py`'s
      `test_link_carries_no_garbage` (bench, task 6.5) is the closer proxy for "no parse
      errors": 0 `BAD_DATA` blocks across every HIL run once the stack sizing settled.

## 4. The USB telemetry stream

- [x] 4.1 Add the send helper: pack into a stack `uint8_t buf[MAVLINK_MAX_PACKET_LEN]`,
      check `availableForWrite() >= len`, write or return false — no allocation, no
      wait; verify with `pio run` and by inspection that no `pvPortMalloc` appears on
      this path. **`buf` is `kMaxOutFrameLen` (80 B), not `MAVLINK_MAX_PACKET_LEN`
      (280 B)** — the latter sizes for the protocol's absolute worst case (a 255-byte
      payload plus a signature block this firmware never sends), while the four shared
      builders top out at `STATUSTEXT`'s 54-byte payload; 80 covers that with margin
      without provisioning 200 B nobody uses. Checked against `msg->len` *before*
      `mavlink_msg_to_send_buffer()` touches the buffer, not after — that function
      writes based on `msg->len` with no bound of its own, so a post-hoc check would be
      validating a write that already happened. No `pvPortMalloc` on this path,
      confirmed by inspection.
- [x] 4.2 Add the `{function, interval_ms, last_ms}` schedule for `HEARTBEAT` and
      `SYSTEM_TIME` at 1 Hz and `BATTERY_STATUS` every 2 s, round-robined one message
      per pass, breaking out of the pass on a failed send; **[board]** verify the rates
      with `mavproxy.py` on USB. Verified via pymavlink over `bench` (no adapter, so
      `mavproxy.py` itself not used — same gap as 2.4/3.3/3.4): 20 s of continuous
      collection measured `HEARTBEAT` and `SYSTEM_TIME` at 20/20 s and `BATTERY_STATUS`
      at 10-11/20 s, matching 1 Hz and 0.5 Hz within the sampling window.
- [x] 4.3 Consume all pending input before emitting telemetry in each pass, so a reply
      is never starved; **[board]** verify a `TIMESYNC` round-trip on USB while the
      stream is running. Verified: sent `TIMESYNC` with `tc1=0` while the schedule was
      actively producing `HEARTBEAT`/`SYSTEM_TIME`/`BATTERY_STATUS`, and the reply
      arrived — the read loop drains all pending bytes before `runTelemetryPass()` runs
      (task 4.3's ordering is structural in `TaskCli`'s loop, not a race), so this holds
      by construction, confirmed rather than assumed.
- [x] 4.4 Give the USB endpoint its own sequence numbering and confirm it is
      independent of `Serial1`'s; **[board]** verify with a ground station on each port
      that neither shows gaps caused by the other. **Independence confirmed by source,
      not by simultaneous dual-port observation** — no USB-TTL adapter this session, so
      Serial1 and USB could not be watched at once (the same gap every Serial1-adjacent
      task in this change hits). What was verified: the plain `mavlink_msg_*_pack()`
      functions this design originally called all route through
      `mavlink_finalize_message()`, which hardcodes `MAVLINK_COMM_0` — using them from
      both ports would have shared one sequence counter regardless of transport. Found
      during implementation, not assumed; see `MavlinkShared.h`'s note and design.md.
      Fixed by switching every builder to the `*_pack_chan()` variant with an explicit
      channel, which threads into `mavlink_get_channel_status(chan)`'s own per-channel
      counter — independence is then a property of the library's per-channel state, not
      of anything this firmware coordinates itself. `HEARTBEAT` sequence numbers on
      `MAVLINK_COMM_0` observed advancing correctly (108, 110, 113, 115, 118, 120 across
      six reads) after the switch, confirming the mechanism works at all.
- [x] 4.5 **[board]** Confirm the identity triple on USB matches the radio link exactly
      — system `1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET` — along with the
      `MAV_AUTOPILOT_GENERIC` field `mavlink-link` also fixes, and that a GCS attached
      to both sees one vehicle. Verified on USB directly (`check_telemetry.py`'s
      identity and vehicle-type cases, run against both the flight build's USB endpoint
      and `bench`'s USB-as-Serial1, both PASS) — the identity triple is hardcoded
      identically in every shared builder regardless of which channel calls it, so it
      cannot differ by port. "A GCS attached to both" is the same simultaneous-port
      gap as 4.4; not literally observed.
- [x] 4.6 Grow the console task's stack in `src/main.cpp` from 128 — `add-console-cli`'s
      archived, board-measured figure, not the 192 this task originally started
      from — to a provisional 256 words to hold the frame buffer; **[board]** replace
      that with the measured need from `ps` and record it. **This task's real story is
      a RAM-budget rebalancing across two tasks, discovered entirely on the board:**
      - 128 words overflowed for real — `Watchdog` reset, `StackOverflowFault` phase,
        reproduced three times (once even at a provisional 224 with the un-consolidated
        buffers) before the fix. `ps` is correctly unreachable once `mavlinkMode` is
        true (spec-mandated), so measuring the peak needed a temporary diagnostic: a
        `STATUSTEXT` reporting `uxTaskGetStackHighWaterMark(NULL)`, added to the real
        (unmodified) mode-locked code path and removed once sizing was done — not a
        bypass of the mode lock, which would have measured an code path that cannot
        happen in production (an earlier attempt did exactly that by accident and
        produced a contaminated, overly dire reading).
      - Fixing the 80-byte send buffer (task 4.1) and consolidating the inbound/outbound
        `mavlink_message_t` into one shared static (task 3.1) brought the peak down
        enough to fit; final measurement with both fixes in place: **160 words, 17 free
        (10.6%)**, board-verified stable across two independent 15-20 s sustained
        MAVLink-mode sessions (telemetry + `TIMESYNC` + a commanded reboot) with zero
        resets. Thinner than this project's usual 35-45% margin — an explicit, informed
        trade-off against an already-tight RAM budget, not an oversight; see
        `src/main.cpp`'s comment on `cliStack` and the RAM section of `proposal.md`.
      - The same story played out for `mavlinkStack` (`src/main.cpp`, unrelated
        declaration, same file): an initial trim to 176 words, based on 109/256 used
        from a handful of manual bench commands, also overflowed — that reading had
        never exercised the `default`/`STATUSTEXT` branch (an unhandled inbound message
        such as `MISSION_REQUEST_LIST`), which turned out to be the deepest path in
        `TaskMavlink` by a wide margin. Re-measured properly (every inbound case,
        including that one, via the same temporary-`STATUSTEXT` technique) at 188/256
        used. Final size **248 words, 60 free (24%)** — affordable once the
        buffer-consolidation savings above freed enough of the RAM budget to widen this
        margin back toward the project's norm rather than leaving it at the first,
        thinner 224. Verified stable: the full HIL suite (0 failures) plus ten repeated
        `MISSION_REQUEST_LIST` sends in a row, specifically to stress the path that
        found the original underestimate.
      - Total RAM headroom after all of the above: **1080 B** (56 B above the 1024 B
        floor) — tight, but real, board-verified, and arrived at by finding genuine
        waste (an oversized buffer, a duplicated library array, two statics that could
        safely be one) rather than by lowering the floor or shipping an unverified size.

## 5. Delete `bench`

- [x] 5.1 Remove `[env:bench]` from `platformio.ini` and the `-D LINK_SERIAL` override
      with it; verify `pio run` builds the remaining environments and that
      `default_envs` and `test_filter` still resolve as intended. Verified: bare
      `pio run` builds only `uno_r4_minima`; `pio run -e libs` still builds; `pio run
      -e bench` now fails with `UnknownEnvNamesError`, confirming the environment is
      gone rather than merely unused.
- [x] 5.2 Simplify `include/Link.h`: `LINK_SERIAL` is no longer an override point,
      `LINK_BAUD` stays one; verify by building and by grepping that nothing still
      defines `LINK_SERIAL` from a build flag. Verified: `grep -rn "LINK_SERIAL"
      platformio.ini` empty; both remaining environments build clean, 0 warnings.
- [x] 5.3 Simplify `include/Cli.h` the same way — `CLI_SERIAL` was introduced by
      `add-console-cli` solely to dodge the `bench` collision, which no longer exists;
      verify with `pio run`. Verified the same way as 5.2, same grep confirms no
      `CLI_SERIAL` build-flag override remains either.

## 6. HIL suite

- [ ] 6.1 Delete `test/test_hil/check_silence.py`: it asserts USB carries no MAVLink
      frames, which this change reverses; verify `run.py --list` no longer offers it
- [ ] 6.2 Add a case asserting the port stays in CLI mode under text and lone header
      bytes, and a case asserting it switches after one valid frame; **[board]** verify
      both with `run.py --filter mode`
- [ ] 6.3 Make the ordering explicit in `run.py`: every case needing CLI mode runs
      before any case that frames a valid MAVLink message on the same port, within the
      one boot `pio test` provides — no reset between groups (task 1.3 found the
      1200-baud touch parks the board in DFU indefinitely rather than giving a cheap
      reset, so it is not used as one); **[board]** verify a full run passes, freshly
      flashed, with the CLI cases genuinely completing before the first MAVLink case
      switches the port
- [ ] 6.4 Retarget the existing MAVLink cases at USB now that the flight build answers
      there, and drop whatever adapter handling is no longer needed; **[board]** verify
      the whole suite passes against the flight build with only a USB cable attached
- [ ] 6.5 **[board]** Run `pio test` and confirm every case passes on the firmware that
      flies — this is the point of the change and the evidence that `bench` was
      genuinely redundant

## 7. Verification on the board

- [ ] 7.1 **[board]** With a host attached to USB and not draining, confirm telemetry
      rates on `Serial1`, the housekeeping cadence and the SD log are all unchanged
- [ ] 7.2 **[board]** Read every task's high-water mark after the console task grew,
      from `ps` and from the SD log, and confirm they agree and none lost its margin
- [ ] 7.3 **[board]** Run `free` after several minutes with both endpoints active and
      confirm the minimum-ever-free heap is no worse than before this change — the
      design claims zero heap cost for the second stream, and this is what proves it

## 8. Documentation

- [ ] 8.1 Update `ARCHITECTURE.md`: two MAVLink endpoints, the mode-detection rule, the
      USB path's drop-on-full behaviour, the deleted `bench`, and the console task's new
      stack; verify by re-reading it against the built firmware
- [ ] 8.2 Update `CLAUDE.md`: `pio test` no longer needs `-e bench`, `pio device
      monitor` reaches a CLI that becomes a MAVLink endpoint, and the MAVProxy
      invocation points at the USB port again
- [ ] 8.3 Run `openspec validate add-usb-dual-protocol --strict` and `pio check` and
      confirm both are clean, with the `console-cli` INFO resolved once
      `add-console-cli` is archived
