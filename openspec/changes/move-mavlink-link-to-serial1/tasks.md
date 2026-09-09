Steps marked **[BOARD]** need the assembled board. `pio run` is the only check that
runs without hardware. Steps marked **[DESTRUCTIVE]** reflash the board or erase the
SD card — ask before running them, and leave the board operational afterwards.

The `LINK_BAUD` open question in `design.md` is settled for implementation as **57600**
(the SiK-radio convention). It is a `build_flag` default, so selecting the radio later
changes one number and nothing else.

## 1. The link definition

- [ ] 1.1 Create `include/Link.h` defining `LINK_SERIAL` (default `Serial1`) and
      `LINK_BAUD` (default `57600`), each wrapped in `#ifndef` so `build_flags` wins;
      verify with `pio run` that it compiles and with
      `pio run -- -D LINK_SERIAL=Serial` that the override is accepted.
- [ ] 1.2 Confirm the alias resolves to the concrete type, not a `HardwareSerial&`:
      grep the tree for `HardwareSerial` and confirm no reference or pointer to the
      link port is introduced anywhere (design.md, Decision 1 — a base-class reference
      silently selects the per-byte `Print::write` path).

## 2. Task bodies

- [ ] 2.1 In `src/serial.cpp`, replace every `Serial` reference with `LINK_SERIAL` in
      both `TaskSerialWrite` and `TaskSerialRead`; verify with `pio run` and by
      grepping the file for a remaining bare `Serial`.
- [ ] 2.2 In `src/serial.cpp`, delete both `while (!Serial) { vTaskDelay(...); }`
      guards; verify no readiness wait remains in the file.
- [ ] 2.3 In `src/mavlink.cpp`, delete `waitSerial()` and its four call sites in
      `TaskHeartbeat` and `TaskMavlinkBatteryStatus`; verify with `pio run` and by
      confirming the file no longer references `Serial` at all — after this change
      `src/mavlink.cpp` should touch no port directly.
- [ ] 2.4 Remove the now-unused `#include <Serial.h>` from `src/mavlink.cpp` if it is
      no longer needed; verify with `pio run`.

## 3. Composition root

- [ ] 3.1 In `src/main.cpp`, add `LINK_SERIAL.begin(LINK_BAUD)` and keep
      `Serial.begin(115200)` for the reserved console; verify with `pio run` that both
      ports are opened and neither is opened twice.
- [ ] 3.2 In `src/main.cpp`, change `TaskSerialWrite`'s priority from
      `PRIORITY_HIGHEST` to `PRIORITY_HIGH`; verify by reading back the `xTaskCreate`
      line and confirming the value comes from `include/Priority.h`, not a literal.
- [ ] 3.3 Add `-D LINK_BAUD=57600` (or leave it to the header default) to
      `platformio.ini` as the documented place to change it; verify with `pio run`.

## 4. Documentation

- [ ] 4.1 Update `ARCHITECTURE.md`: the hardware map gains D0/D1 as the link port and
      records `Serial` (USB) as a console with **no owner**; §5.1 stops saying
      `src/serial.cpp` owns "the UART" ambiguously and names the link port; the note
      about tasks waiting on `while (!Serial)` is removed. Verify by re-reading §3, §5.1
      and §6 against the code.
- [ ] 4.2 Update `ARCHITECTURE.md` §3: the task table's priority for `TaskSerialWrite`
      becomes HIGH, and the sentence justifying HIGHEST for serial I/O is narrowed to
      the read side, with the `UART::write` busy-wait as the reason the writer is not
      HIGHEST. Verify the table and the prose agree.
- [ ] 4.3 Update `README.md`: hardware table gains the UART link row and marks USB as
      a console; the `mavproxy.py --master=/dev/ttyACM0,115200` line is replaced with
      the radio/adapter port at 57600, and the `-D LINK_SERIAL=Serial` fallback is
      documented for bench work. Verify by following the README's own quick start.

## 5. Verification

- [ ] 5.1 `pio run` — the only check that runs without hardware. Must pass.
- [ ] 5.2 `pio run -- -D LINK_SERIAL=Serial` — confirm the revert path still builds.
- [ ] 5.3 **[BOARD] [DESTRUCTIVE]** Flash the `-D LINK_SERIAL=Serial` build and run
      `mavproxy.py --master=/dev/ttyACM0,115200 --load-module system_time`. Confirm one
      vehicle appears as system `1` / `MAV_COMP_ID_AUTOPILOT1` / `MAV_TYPE_ROCKET`,
      `HEARTBEAT` and `SYSTEM_TIME` arrive at 1 Hz, `BATTERY_STATUS` every 2 s, and
      setting the clock from the GCS still works. This proves the refactor did not
      break the protocol.
- [ ] 5.4 **[BOARD] [DESTRUCTIVE]** With that same build running, pull the housekeeping
      log from the SD card and compare the seven high-water marks against a pre-change
      log. Deleting `waitSerial()` and changing the write path both move stack usage;
      confirm no task lost margin, paying particular attention to `TaskSerialWrite`
      (192 w) and `TaskLogger` (96 w, the tightest). This is the only evidence the
      stacks still fit.
- [ ] 5.5 **[BOARD] [DESTRUCTIVE]** Confirm the board reaches steady state with no USB
      host attached — the `while (!Serial)` guards are gone, so nothing should stall.
      Power it from the battery alone and verify from the SD log afterwards that
      records were written at 1 Hz from boot.
- [ ] 5.6 **[BOARD] [DESTRUCTIVE]** Re-flash the default (`Serial1`) build with
      `pio run -t upload` so the board is left in its operational configuration.

## 6. Gaps to state, not to close

- [ ] 6.1 Record in the change that **the UART path itself was not exercised**: no
      USB-TTL adapter and no radio are available, so §5 verifies everything except
      bytes actually moving on D0/D1. Do not report this change as verified on
      hardware without that caveat.
- [ ] 6.2 Note that `pio test` is **not** informative for this change. It touches no
      `lib/` code, and `test_build_src` defaults to `no`, so `src/` is not in the test
      binary and no task body is covered. Running it would reflash the board and erase
      `data*.mpk` and `index.bin` while proving nothing about these edits. The
      high-water-mark check in 5.4 is the substitute, and it is what the project's task
      rule is actually reaching for.
