# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Firmware for a CubeSat (hardware based on https://www.thingiverse.com/thing:4096437). Arduino UNO R4 Minima (Renesas RA) + FreeRTOS, built with PlatformIO. The board presents itself to a ground station over USB serial as a MAVLink vehicle; MAVProxy is the reference GCS:

```bash
mavproxy.py --master=/dev/ttyACM0,115200 --load-module system_time
```

Planned and in-progress work is tracked in [TODO.md](TODO.md); check it before starting a
feature, and add or update the entry there when one is defined or finished.

## Commands

```bash
pio run                  # build (this is all CI runs)
pio run -t upload        # flash the board
pio device monitor       # serial console at 115200 (raw MAVLink bytes, not text)
pio test                 # Unity tests — ON DEVICE ONLY, needs board + DS1307 + SD card
```

There is no host/native test environment: `test/test_main.cpp` asserts against real battery voltage, RTC and SD hardware, so tests cannot run in CI or on a dev machine. All test cases live in one file and are dispatched from a hand-written `runUnityTests()`; to run a single case, comment out the other `RUN_TEST(...)` lines — `pio test -f` filters test *directories*, of which there is only one.

## Architecture

`src/main.cpp` is the only place tasks and queues are created; everything else is a task body in its own translation unit reaching shared objects via `extern`. Adding a subsystem means: define the task in a new `src/*.cpp`, declare it `[[noreturn]] extern` in `main.cpp`, and `xTaskCreate` it there.

**Queues carry heap pointers, never values.** Producers `pvPortMalloc` a `Data*` or `mavlink_message_t*`, and every `xQueueSend` must `vPortFree` on `!= pdPASS` or the RAM leaks — the MCU has no memory to spare. The consumer owns the pointer and frees it after use. Follow this protocol exactly in new code.

Three pipelines, all fed by queues:

- **Serial ↔ MAVLink.** `serial.cpp` owns the UART: `TaskSerialRead` byte-parses into `serialReadQueue`, `TaskSerialWrite` drains `serialWriteQueue`. `mavlink.cpp` holds all protocol logic — `TaskMavlink` dispatches inbound by `msgid`, while `TaskHeartbeat` and `TaskMavlinkBatteryStatus` emit periodic telemetry. Nothing outside `serial.cpp` should touch `Serial` for I/O; tasks only wait on `while (!Serial)` before publishing.
- **Housekeeping log.** `logger.cpp` samples free heap and each task's stack high-water mark once a second into a `Data` struct (`include/Data.h`) and posts it to `sdWriteQueue`; `sdwrite.cpp` converts it to a `JsonDocument` and hands it to `SdData`.
- **Time.** `lib/SystemTime` keeps the R4's internal `RTC` and an external DS1307 in sync — `begin()` seeds the internal clock from the DS1307, `setUnixTime()` writes both and short-circuits when already correct. Inbound `SYSTEM_TIME` and `TIMESYNC` from the GCS drive it, so the satellite's clock is settable from the ground.

Identity on the bus is hardcoded: system id `1`, component `MAV_COMP_ID_AUTOPILOT1`, type `MAV_TYPE_ROCKET`. Any new outbound message must use the same triple.

`lib/SdData` is a fixed-footprint ring of `data0..N.mpk` files with the current index persisted in `index.bin`, so a power cycle resumes where it left off and the card can never fill. Despite taking an `ArduinoJson` document it serializes **MessagePack**, not JSON — the `.mpk` files are binary.

`lib/Battery` wraps the `SolarCharger` library on `A0` and caches reads for 125 ms; `remaining()` is a naive linear 3.5 V–4.2 V LiPo map.

### Constraints that shape the code

- Stack sizes in `xTaskCreate` are in **words**, and are tuned tight (96–256). `configCHECK_FOR_STACK_OVERFLOW=2` is on and `src/hooks.cpp` traps an overflow into a 0.5 Hz `LED_BUILTIN` blink with interrupts disabled — a board blinking slowly on boot means a stack is too small, not a wiring fault. The per-task high-water marks in the SD log exist to size these; check them after changing any task body.
- Task priorities come from `include/Priority.h` (`PRIORITY_LOWEST`..`PRIORITY_HIGHEST`), not raw numbers. Serial I/O is HIGHEST, SD writing LOWEST.
- Wiring is hardcoded, not configurable: SD card CS on pin **9**, battery sense on **A0**, DS1307 on I2C.
- `setup()` uses `configASSERT` for RTC, SD and queue creation, so missing hardware halts the board rather than degrading.
