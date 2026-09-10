Steps marked **[BOARD]** need the assembled board. `pio run` is the only check that
runs without hardware. Steps marked **[DESTRUCTIVE]** reflash the board or erase the
SD card — ask before running them, and leave the board operational afterwards.

The `LINK_BAUD` open question in `design.md` is settled for implementation as **57600**
(the SiK-radio convention). It is a `build_flag` default, so selecting the radio later
changes one number and nothing else.

## 1. The link definition

- [x] 1.1 Create `include/Link.h` defining `LINK_SERIAL` (default `Serial1`) and
      `LINK_BAUD` (default `57600`), each wrapped in `#ifndef` so `build_flags` wins;
      verify with `pio run` that it compiles and with
      `PLATFORMIO_BUILD_FLAGS="-D LINK_SERIAL=Serial" pio run` that the override is
      accepted.
- [x] 1.2 Confirm the alias resolves to the concrete type, not a `HardwareSerial&`:
      grep the tree for `HardwareSerial` and confirm no reference or pointer to the
      link port is introduced anywhere (design.md, Decision 1 — a base-class reference
      silently selects the per-byte `Print::write` path).

## 2. Task bodies

- [x] 2.1 In `src/serial.cpp`, replace every `Serial` reference with `LINK_SERIAL` in
      both `TaskSerialWrite` and `TaskSerialRead`; verify with `pio run` and by
      grepping the file for a remaining bare `Serial`.
- [x] 2.2 In `src/serial.cpp`, delete both `while (!Serial) { vTaskDelay(...); }`
      guards; verify no readiness wait remains in the file.
- [x] 2.3 In `src/mavlink.cpp`, delete `waitSerial()` and its three call sites in
      `TaskHeartbeat` and `TaskMavlinkBatteryStatus`; verify with `pio run` and by
      confirming the file no longer references `Serial` at all — after this change
      `src/mavlink.cpp` should touch no port directly.
- [x] 2.4 Remove the now-unused `#include <Serial.h>` from `src/mavlink.cpp` if it is
      no longer needed; verify with `pio run`.

## 3. Composition root

- [x] 3.1 In `src/main.cpp`, add `LINK_SERIAL.begin(LINK_BAUD)` and keep
      `Serial.begin(115200)` for the reserved console; verify with `pio run` that both
      ports are opened and neither is opened twice.
- [x] 3.2 In `src/main.cpp`, change `TaskSerialWrite`'s priority from
      `PRIORITY_HIGHEST` to `PRIORITY_HIGH`; verify by reading back the `xTaskCreate`
      line and confirming the value comes from `include/Priority.h`, not a literal.
- [x] 3.3 Add `-D LINK_BAUD=57600` (or leave it to the header default) to
      `platformio.ini` as the documented place to change it; verify with `pio run`.
      Done as commented override examples rather than an active `-D`, so the default
      has one home (`include/Link.h`) instead of two that can drift apart.

## 4. Documentation

- [x] 4.1 Update `ARCHITECTURE.md`: the hardware map gains D0/D1 as the link port and
      records `Serial` (USB) as a console with **no owner**; §5.1 stops saying
      `src/serial.cpp` owns "the UART" ambiguously and names the link port; the note
      about tasks waiting on `while (!Serial)` is removed. Verify by re-reading §3, §5.1
      and §6 against the code.
- [x] 4.2 Update `ARCHITECTURE.md` §3: the task table's priority for `TaskSerialWrite`
      becomes HIGH, and the sentence justifying HIGHEST for serial I/O is narrowed to
      the read side, with the `UART::write` busy-wait as the reason the writer is not
      HIGHEST. Verify the table and the prose agree.
- [x] 4.3 Update `README.md`: hardware table gains the UART link row and marks USB as
      a console; the `mavproxy.py --master=/dev/ttyACM0,115200` line is replaced with
      the radio/adapter port at 57600, and the `-D LINK_SERIAL=Serial` fallback is
      documented for bench work. Verify by following the README's own quick start.

## 5. Verification

- [x] 5.1 `pio run` — the only check that runs without hardware. Must pass.
- [x] 5.2 `PLATFORMIO_BUILD_FLAGS="-D LINK_SERIAL=Serial" pio run` — confirm the
      revert path still builds.
- [x] 5.3 **[BOARD] [DESTRUCTIVE]** Flash the `-D LINK_SERIAL=Serial` build and run
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
- [x] 5.6 **[BOARD] [DESTRUCTIVE]** Re-flash the default (`Serial1`) build with
      `pio run -t upload` so the board is left in its operational configuration.

## 6. Gaps to state, not to close

- [x] 6.1 Record in the change that **the UART path itself was not exercised**: no
      USB-TTL adapter and no radio are available, so §5 verifies everything except
      bytes actually moving on D0/D1. Do not report this change as verified on
      hardware without that caveat.
- [x] 6.2 Note that `pio test` is **not** informative for this change. It touches no
      `lib/` code, and `test_build_src` defaults to `no`, so `src/` is not in the test
      binary and no task body is covered. Running it would reflash the board and erase
      `data*.mpk` and `index.bin` while proving nothing about these edits. The
      high-water-mark check in 5.4 is the substitute, and it is what the project's task
      rule is actually reaching for.


## Verification record

Run on 2026-09-10 against the assembled board on `/dev/ttyACM0`, with MAVProxy 1.8.74
installed via pipx (`future` and `setuptools` had to be injected into the venv — a
packaging gap on Python 3.14). Checks were driven with `pymavlink` directly rather
than MAVProxy's interactive console, so they are scriptable and repeatable.

**What was run and passed.** With the `-D LINK_SERIAL=Serial` build flashed
(task 5.3), listening for 15 s:

| Observed | Expected | Result |
|---|---|---|
| `HEARTBEAT` 1.04 Hz | 1 Hz | pass |
| `SYSTEM_TIME` 0.98 Hz | 1 Hz | pass |
| `BATTERY_STATUS` 0.52 Hz | every 2 s | pass |
| identity `(1, 1)`, heartbeat `type` 9 | sysid 1, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET` | pass |
| inbound `SYSTEM_TIME` accepted | clock settable from the GCS | pass |
| `TIMESYNC` with `tc1 == 0` answered, `ts1` echoed intact | reply with satellite time | pass |

The satellite clock was 103 days behind (2026-05-29) and was set to real time as part
of that check. That is a deliberate state change to the DS1307, not a side effect.

With the default `Serial1` build flashed (task 5.6), USB is **silent** — zero MAVLink
messages and zero raw bytes over 12 s and 8 s respectively. Taken with the previous
result, this is direct evidence that the link left USB.

**What was NOT run, and why.**

- **The UART itself is unverified.** No USB-TTL adapter and no radio, so nothing
  confirms bytes actually move on D0/D1. USB going silent proves the link left USB; it
  does not prove it arrived anywhere. This change must not be described as verified on
  hardware without that caveat.
- **Task 5.4 (stack high-water marks) was not done.** The marks are only written to the
  SD card, the card is inside the satellite, and there is no way to read it over the
  link. The stacks are therefore *unmeasured* after this change. `TaskSerialWrite`
  (192 w) lost the `while (!Serial)` frame and `TaskHeartbeat` /
  `TaskMavlinkBatteryStatus` (128 w each) lost `waitSerial()`, so usage should have
  gone down, not up — but that is reasoning, not evidence.
- **Task 5.5 (battery-only boot) was not done.** It needs physical access to power the
  board without USB.
- **`pio test` was deliberately not run** (task 6.2). It touches no `lib/` code, and
  `test_build_src` defaults to `no`, so `src/` is not in the test binary and no task
  body is covered. It would have reflashed the board and erased `data*.mpk` and
  `index.bin` for no information.

**One incident worth recording.** Before flashing, the board enumerated as a sketch
but emitted nothing on USB. To tell a stale binary apart from a halt in
`configASSERT(systemTime.begin())`, a 1200-baud touch was used to restart it. On this
board that enters DFU and **stays there** — it does not time out into the sketch as
AVR boards do. The board was recovered by flashing. The question it was meant to answer
was then settled anyway: the new firmware runs past both `configASSERT` calls, so the
DS1307 and the SD card both answer, and the earlier silence was a stale binary — most
likely the Unity test binary from an old `pio test`, which prints once at boot and then
idles. Do not use the 1200-baud touch on this board as a reset.
