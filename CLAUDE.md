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
pio run                  # build the flight environment (CI builds all three)
pio run -t upload        # flash the board
pio device monitor       # serial console at 115200 (raw MAVLink bytes, not text)
pio test                 # HIL: flashes this firmware, then checks it from the host
pio test -e bench        # same, with the link on USB so no adapter is needed
pio test -e libs         # Unity library tests — DESTRUCTIVE, see below
```

There is no host/native test environment. `test/` holds two suites, and **`pio test`
means the HIL one**: it flashes this firmware and then interrogates it from the host,
leaving the board running what it would fly.

- **`test/test_hil/`** — host-side Python driving the flashed firmware over the MAVLink link, via `test/test_hil/run.py`. Nine cases, aligned with the scenarios in `openspec/specs/mavlink-link/spec.md`. Needs `pymavlink` (`pip install -r test/test_hil/requirements.txt`) and the link reachable: with the flight build that means a USB-TTL adapter on D0/D1, so use `pio test -e bench` to put the link on USB instead. `run.py --list` and `--filter` run a single case by name.
- **`test/test_libs/`** — the Unity suite. `test_main.cpp` asserts against real battery voltage, RTC and SD hardware, so it never runs in CI. It runs on a dev machine with the board attached — `pio device list` shows a `UNO R4 Minima - CDC Port` — taking about 25 s for the 5 cases. All cases live in that one file, dispatched from a hand-written `runUnityTests()`; to run a single case, comment out the other `RUN_TEST(...)` lines. `pio test -f` filters test *directories*, so it picks a suite, not a case.
- **`test/test_hil/`** — host-side Python that interrogates the flashed firmware over the MAVLink link. `platformio.ini` excludes it with `test_ignore = test_hil`, since PlatformIO would try to compile it as C++. Run these by hand; see its `README.md`.

**`test_libs` covers `lib/` and nothing else.** In that environment `test_build_src` is off, so `src/` is not in the test binary: no task, no queue, no scheduler, no stack high-water mark. A green `pio test -e libs` proves nothing about a task body — only the SD log's high-water marks or a HIL check can.

Adding any `test_*` subdirectory is what makes PlatformIO stop treating `test/` itself as a suite: it falls back to the root only when there are none. Keep every suite in its own directory, or one of them stops running with no warning.

**`pio test -e libs` is the destructive one**: it replaces the firmware with the Unity binary, and `cleanSdFiles()` deletes `data*.mpk` and `index.bin` from the card on every case. Ask before running it, and follow with `pio run -t upload` to leave the board operational. That is why it is opt-in and HIL is the default — `pio test` and `pio test -e bench` flash real firmware and leave the board running, and neither touches the card. After `-e bench` the link is on USB rather than D0/D1, so `pio run -t upload` restores the flight configuration.

Three environments, one board: `uno_r4_minima` is what flies and is the default for every command; `bench` is the same firmware with the link on USB; `libs` exists only to run the Unity suite. `default_envs` keeps a bare `pio run`, `pio check` or `pio test` on the first of them.

A change that touches `lib/` or a task body is not verified by building it. Run the tests on the board, or say plainly that you did not.

## Conventions when editing

These are the invariants that are easiest to break silently. `ARCHITECTURE.md` explains why each one exists.

- **Adding a subsystem is four edits:** the task body in a new `src/*.cpp` reaching shared objects via `extern`, a `[[noreturn]] extern` declaration in `src/main.cpp`, its static storage (`StackType_t xStack[N]` and a `StaticTask_t`) beside it, and an `xTaskCreateStatic` in `setup()`. Tasks and queues are created nowhere else, and never with the dynamic `xTaskCreate` / `xQueueCreate` — CI greps `src/` for both and fails. Note `portable/FSP/port.c` contains its own `xTaskCreate` of 1024 words: it is unreachable only because no build here defines `AUTOSTART_FREERTOS` or `EARLY_AUTOSTART_FREERTOS`, and with the current heap it could not succeed. Do not define either.
- **Queues carry heap pointers, never values.** Producer `pvPortMalloc`s, checks the result for `NULL`, and `vPortFree`s if `xQueueSend` does not return `pdPASS`. The consumer frees after use. A leak is still fatal within minutes: the heap is `0x1800` and backs the queued items only. Changing a queue's depth means re-deriving what backs it — depth plus one block per producer that can hold an unsent item and one per consumer holding an unreleased one, not depth alone.
- **Stack sizes in `xTaskCreateStatic` are words, not bytes**, and are tuned tight (96–256). The count must match the length of the `StackType_t` array passed alongside it. After changing a task body, check that task's high-water mark in the SD log before assuming it still fits.
- **Priorities come from `include/Priority.h`**, never raw numbers.
- **Only the owning file touches its resource:** `src/serial.cpp` the UART, `src/sdwrite.cpp` the card, `lib/SystemTime` the clocks, `lib/Battery` the ADC. Everything else goes through a queue or the library wrapper. This is what makes the absence of mutexes safe — do not break it by reaching for a peripheral directly.
- **Every outbound MAVLink message uses the same identity triple:** system id `1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.
- **Wiring is hardcoded** (SD `CS` on 9, battery on `A0`, DS1307 on I2C). If a change adds a pin, document it in `ARCHITECTURE.md`.
- **`configASSERT` in `setup()` halts the board on purpose** for missing RTC, SD, queues or tasks. Do not soften it into a degraded boot without an explicit decision.
- **The RAM budget is a build-time fact.** `scripts/ram_budget.py` runs after every link, prints the true commitment — which the `RAM:` line does not, omitting 9472 bytes — and fails the build when headroom drops below `custom_ram_min_headroom`. A change that does not fit fails on your desk, which is the point; do not lower the floor to make one pass.
- When a change makes `ARCHITECTURE.md` inaccurate, update it in the same commit.
