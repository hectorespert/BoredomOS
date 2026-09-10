# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Firmware for a CubeSat (hardware based on https://www.thingiverse.com/thing:4096437). Arduino UNO R4 Minima (Renesas RA) + FreeRTOS, built with PlatformIO. The board presents itself to a ground station over USB serial as a MAVLink vehicle; MAVProxy is the reference GCS:

```bash
mavproxy.py --master=/dev/ttyACM0,115200 --load-module system_time
```

**Read [ARCHITECTURE.md](ARCHITECTURE.md) before changing anything.** It is the single source of truth for the design: the task and queue model, the three pipelines, which file owns which resource, and the constraints that explain why the code looks the way it does. This file does not repeat it.

Planned work is tracked in [TODO.md](TODO.md); check it before starting a feature, and
add an entry there when one is defined. Entries are written in English, one per feature
or defect, and defined before any code is written. `TODO.md` holds only work that has
**not** been picked up, so an entry is never `in progress`. The `Done` section at the
end predates this rule and is kept as a record; nothing new is added to it.

Three places hold planning, and they do not overlap. **`ARCHITECTURE.md` is structure and
why** — how the firmware is put together, and the constraints behind it. **`TODO.md` is the
backlog** — everything planned that nobody is implementing right now. **`openspec/` is the
change in flight**: when a `TODO.md` entry is picked up, `/opsx:propose` turns it into a
change under `openspec/changes/` with its proposal, design, spec deltas and tasks, and the
`TODO.md` entry is **deleted in the same commit**. The change now owns that work; leaving a
copy behind in the backlog means two descriptions of the same thing drifting apart.

Deleting it has a consequence worth stating: nothing is left to mark *Done* later, so
`openspec/specs/` and the git history are the record of what was built. `openspec/specs/`
fills up as changes are archived rather than being written up front. Before deleting an
entry, re-point any other entry that cross-references it at the change id, or the reference
dangles.

Do not open a change for an entry that is not being implemented, and do not restate the
architecture in a spec.

## Commands

```bash
pio run                  # build (this is all CI runs)
pio run -t upload        # flash the board
pio device monitor       # serial console at 115200 (raw MAVLink bytes, not text)
pio test                 # Unity tests — ON DEVICE ONLY, needs board + DS1307 + SD card
```

There is no host/native test environment: `test/test_main.cpp` asserts against real battery voltage, RTC and SD hardware, so tests never run in CI. They do run on a dev machine with the board attached — `pio device list` shows a `UNO R4 Minima - CDC Port` — taking about 25 s for the 5 cases. All test cases live in one file and are dispatched from a hand-written `runUnityTests()`; to run a single case, comment out the other `RUN_TEST(...)` lines — `pio test -f` filters test *directories*, of which there is only one.

`pio test` is not a read-only check: it reflashes the board with the test binary, and `cleanSdFiles()` deletes `data*.mpk` and `index.bin` from the card on every case. Ask before running it, and follow with `pio run -t upload` to leave the board operational.

A change that touches `lib/` or a task body is not verified by building it. Run the tests on the board, or say plainly that you did not.

## Conventions when editing

These are the invariants that are easiest to break silently. `ARCHITECTURE.md` explains why each one exists.

- **Adding a subsystem is three edits:** the task body in a new `src/*.cpp` reaching shared objects via `extern`, a `[[noreturn]] extern` declaration in `src/main.cpp`, and an `xTaskCreate` there. Tasks and queues are created nowhere else.
- **Queues carry heap pointers, never values.** Producer `pvPortMalloc`s, checks the result for `NULL`, and `vPortFree`s if `xQueueSend` does not return `pdPASS`. The consumer frees after use. On 8 KB of heap a leak is fatal within minutes.
- **Stack sizes in `xTaskCreate` are words, not bytes**, and are tuned tight (96–256). After changing a task body, check that task's high-water mark in the SD log before assuming it still fits.
- **Priorities come from `include/Priority.h`**, never raw numbers.
- **Only the owning file touches its resource:** `src/serial.cpp` the UART, `src/sdwrite.cpp` the card, `lib/SystemTime` the clocks, `lib/Battery` the ADC. Everything else goes through a queue or the library wrapper. This is what makes the absence of mutexes safe — do not break it by reaching for a peripheral directly.
- **Every outbound MAVLink message uses the same identity triple:** system id `1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.
- **Wiring is hardcoded** (SD `CS` on 9, battery on `A0`, DS1307 on I2C). If a change adds a pin, document it in `ARCHITECTURE.md`.
- **`configASSERT` in `setup()` halts the board on purpose** for missing RTC, SD or queues. Do not soften it into a degraded boot without an explicit decision.
- When a change makes `ARCHITECTURE.md` inaccurate, update it in the same commit.
