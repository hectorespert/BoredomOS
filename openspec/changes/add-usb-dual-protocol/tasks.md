Steps marked **[board]** need the assembled board attached. `pio run` is the only
check that runs without hardware.

**This change lands after `add-console-cli`.** It edits the console task that change
creates, and its `console-cli` spec delta modifies requirements that only exist once
`add-console-cli` is archived — `openspec validate` reports that as an INFO today and
it clears itself in that order.

## 1. Measure before building

- [ ] 1.1 **[board]** Measure what `Serial.availableForWrite()` reports on this CDC
      implementation in three states — host attached and draining, host attached and
      not draining, no host — and record the figures in the commit message; the whole
      non-blocking design rests on this being meaningful
- [ ] 1.2 **[board]** Confirm the reported figure is at least the size of the largest
      periodic frame (`BATTERY_STATUS`, ~54 bytes on the wire); if it is smaller than a
      frame the drop-and-retry loop would never fire and the design needs revisiting
      before any of section 3 is written
- [ ] 1.3 **[board]** Confirm the 1200-baud touch resets the board on this core, since
      the HIL ordering in section 5 depends on having a reset mechanism

## 2. Share the message logic

- [ ] 2.1 Add `include/Mavlink.h` declaring the four builders and
      `mavlinkHandleInbound`, each filling a `mavlink_message_t` the caller provides;
      verify with `pio run`
- [ ] 2.2 Split `sendHeartbeat`, `sendSystemTime`, `sendBatteryStatus` and
      `sendStatusText` in `src/mavlink.cpp` into a builder plus the existing allocate-
      and-queue step, keeping the heap block as the buffer the builder fills so no
      128-word producer task gains a 291-byte local; verify with `pio run` and
      **[board]** confirm the high-water marks of both producer tasks are unchanged
- [ ] 2.3 Move the inbound `switch` out of `TaskMavlink` into `mavlinkHandleInbound`,
      returning the reply through a caller-provided buffer, and reduce `TaskMavlink` to
      receive / dispatch / queue-the-reply / free; **[board]** verify with
      `mavproxy.py` on the radio link that `TIMESYNC` and `SYSTEM_TIME` behave exactly
      as before the refactor
- [ ] 2.4 **[board]** Confirm the radio link's behaviour is byte-identical to before
      this section: same messages, same rates, same identity — this refactor must be
      invisible on `Serial1`

## 3. Mode detection

- [ ] 3.1 Add a second parser state to `src/cli.cpp` — `MAVLINK_COMM_1` with its own
      `mavlink_message_t` and `mavlink_status_t` in `.bss` — and feed every inbound
      byte to it before appending to the command buffer; verify with `pio run`
- [ ] 3.2 Switch to MAVLink mode only when `mavlink_parse_char` reports a complete
      frame, never on a header byte, and process the bytes already buffered as MAVLink
      once switched; **[board]** verify by pasting a lone `0xFD` and confirming the CLI
      still answers
- [ ] 3.3 Make the switch one-way: no timer, no escape, no command returns to CLI mode;
      **[board]** verify that after a MAVProxy session the port ignores command text and
      that a reset restores the CLI
- [ ] 3.4 Stop emitting CLI text once in MAVLink mode, so a frame parser never has to
      discard a prompt; **[board]** verify with `mavproxy.py --master=/dev/ttyACM0` that
      no parse errors are reported

## 4. The USB telemetry stream

- [ ] 4.1 Add the send helper: pack into a stack `uint8_t buf[MAVLINK_MAX_PACKET_LEN]`,
      check `availableForWrite() >= len`, write or return false — no allocation, no
      wait; verify with `pio run` and by inspection that no `pvPortMalloc` appears on
      this path
- [ ] 4.2 Add the `{function, interval_ms, last_ms}` schedule for `HEARTBEAT` and
      `SYSTEM_TIME` at 1 Hz and `BATTERY_STATUS` every 2 s, round-robined one message
      per pass, breaking out of the pass on a failed send; **[board]** verify the rates
      with `mavproxy.py` on USB
- [ ] 4.3 Consume all pending input before emitting telemetry in each pass, so a reply
      is never starved; **[board]** verify a `TIMESYNC` round-trip on USB while the
      stream is running
- [ ] 4.4 Give the USB endpoint its own sequence numbering and confirm it is
      independent of `Serial1`'s; **[board]** verify with a ground station on each port
      that neither shows gaps caused by the other
- [ ] 4.5 **[board]** Confirm the identity triple on USB matches the radio link exactly
      — system `1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`,
      `MAV_AUTOPILOT_GENERIC` — and that a GCS attached to both sees one vehicle
- [ ] 4.6 Grow the console task's stack in `src/main.cpp` from 192 to a provisional 384
      words to hold the frame buffer; **[board]** replace that with the measured need
      from `ps` and record it

## 5. Delete `bench`

- [ ] 5.1 Remove `[env:bench]` from `platformio.ini` and the `-D LINK_SERIAL` override
      with it; verify `pio run` builds the remaining environments and that
      `default_envs` and `test_filter` still resolve as intended
- [ ] 5.2 Simplify `include/Link.h`: `LINK_SERIAL` is no longer an override point,
      `LINK_BAUD` stays one; verify by building and by grepping that nothing still
      defines `LINK_SERIAL` from a build flag
- [ ] 5.3 Simplify `include/Cli.h` the same way — `CLI_SERIAL` was introduced by
      `add-console-cli` solely to dodge the `bench` collision, which no longer exists;
      verify with `pio run`

## 6. HIL suite

- [ ] 6.1 Delete `test/test_hil/check_silence.py`: it asserts USB carries no MAVLink
      frames, which this change reverses; verify `run.py --list` no longer offers it
- [ ] 6.2 Add a case asserting the port stays in CLI mode under text and lone header
      bytes, and a case asserting it switches after one valid frame; **[board]** verify
      both with `run.py --filter mode`
- [ ] 6.3 Make the ordering explicit in `run.py`: the CLI cases run before any case
      that frames MAVLink on the same port, or the runner resets the board between the
      two groups via the 1200-baud touch; **[board]** verify a full run passes twice in
      a row without a manual reset between them
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
