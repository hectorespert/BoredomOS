# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Firmware for a CubeSat (hardware based on https://www.thingiverse.com/thing:4096437). Arduino UNO R4 Minima (Renesas RA) + FreeRTOS, built with PlatformIO. The board presents itself to a ground station over USB serial as a MAVLink vehicle; MAVProxy is the reference GCS:

```bash
mavproxy.py --master=/dev/ttyACM0 --load-module system_time
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

A change is the four artifacts `/opsx:propose` names — proposal, specs, design, tasks —
using the default `spec-driven` schema. `openspec/config.yaml` carries the rules for each
artifact and the guidance for apply and archive.

There used to be a heavier custom schema, `boredomos`, forked so the chain neither started
at the task list nor ended at it: a qa-written `test-plan.md` ahead of `tasks.md`, one row
per scenario naming its method and receipt; a `review.md` from six domain-specific reviewer
agents under `.claude/agents/`, blocking `apply` until it existed; and a `verify.md` audit
after apply, closing on a `DECISION:` line before archiving. **It is retired** — the schema
and the six-agent roster both cost more than this project can sustain running on every
change. Changes now use `spec-driven` and are reviewed, applied and archived directly,
without that apparatus, which means without the independent-review guarantee it existed
for — read anything written under this arrangement with that in mind.

`openspec/changes/archive/2026-09-13-add-degraded-mode/` still carries a `test-plan.md` and a `review.md` from
when it used the old schema, migrated to `spec-driven` after the fact. They are not
tracked artifacts anymore, just historical record, but `tasks.md` in that change cites
their row ids and finding numbers throughout — don't delete either file without checking
what in `tasks.md` goes dark.

## Commands

```bash
pio run                  # build the flight environment (CI builds both)
pio run -t upload        # flash the board
pio test                 # HIL: flashes this firmware, then checks it from the host
pio test -e libs         # Unity library tests — DESTRUCTIVE, see below
```

There is no text console. `replace-console-cli-with-usb-mavlink-link` deleted
`src/cli.cpp` and made USB a second MAVLink endpoint, so `pio device monitor`
now shows binary frames. The figures `ps` and `free` used to print — free heap,
minimum-ever-free heap and each live task's stack high-water mark — reach the
ground as the `NAMED_VALUE_INT` housekeeping stream, which a GCS arms with
`MAV_CMD_SET_MESSAGE_INTERVAL` on message id 252 and which is off after every
reset. `src/link.cpp` owns both ports; see `ARCHITECTURE.md` section 6 for the
one exception.

There is no host/native test environment. `test/` holds two suites, and **`pio test`
means the HIL one**: it flashes this firmware and then interrogates it from the host,
leaving the board running what it would fly.

- **`test/test_hil/`** — host-side Python driving the flashed firmware over the MAVLink link, via `test/test_hil/run.py`. 33 cases across seven `check_*.py` modules, most of them about the `mavlink-link` capability. Eight self-skip by default: four in `check_dual_link.py` need an adapter, two need the board put in the reduced configuration by hand, one needs a RESET press, and `check_clock.py`'s restart case reboots the board so it is gated behind `HIL_CLOCK_RESET=1`. Which scenario each case covers is **not recorded** — see *Record which scenario each HIL case covers* in [TODO.md](TODO.md). Needs `pymavlink` (`pip install -r test/test_hil/requirements.txt`). The default target is USB, which the flight build always answers, so no adapter is needed. `HIL_PORT` takes a **device path**, not a port name, so point it at an adapter (`HIL_PORT=/dev/ttyUSB0`) to drive D0/D1 instead; `check_dual_link.py` takes `HIL_UART_PORT` for the cases that need both ports at once and self-skips without it. `run.py --list` and `--filter` run a single case by name.
- **`test/test_libs/`** — the Unity suite. `test_main.cpp` asserts against real battery voltage, RTC and SD hardware, so it never runs in CI. It runs on a dev machine with the board attached — `pio device list` shows a `UNO R4 Minima - CDC Port` — taking on the order of half a minute for the 12 cases. All cases live in that one file, dispatched from a hand-written `runUnityTests()`; to run a single case, comment out the other `RUN_TEST(...)` lines. `pio test -f` filters test *directories*, so it picks a suite, not a case.
- **`test/test_hil/`** — host-side Python that interrogates the flashed firmware over the MAVLink link. `platformio.ini` excludes it with `test_ignore = test_hil`, since PlatformIO would try to compile it as C++. Run these by hand; see its `README.md`.

**`test_libs` covers `lib/` and nothing else.** In that environment `test_build_src` is off, so `src/` is not in the test binary: no task, no queue, no scheduler, no stack high-water mark. A green `pio test -e libs` proves nothing about a task body — only the SD log's high-water marks or a HIL check can.

Adding any `test_*` subdirectory is what makes PlatformIO stop treating `test/` itself as a suite: it falls back to the root only when there are none. Keep every suite in its own directory, or one of them stops running with no warning.

**`pio test -e libs` is the destructive one**: it replaces the firmware with the Unity binary, and `cleanSdFiles()` deletes `data*.BIN` and `index.bin` from the card on every case. It does **not** delete `data*.mpk`, so any log written before `replace-messagepack-log-with-dataflash` survives it — those files are in the retired MessagePack format and nothing in the firmware or the tests touches them again. Ask before running it, and follow with `pio run -t upload` to leave the board operational. That is why it is opt-in and HIL is the default — `pio test` flashes real firmware and leaves the board running, and does not touch the card.

Two environments, one board: `uno_r4_minima` is what flies and is the default for every command, and `libs` exists only to run the Unity suite. `bench` is gone — USB carries MAVLink in every build now, so the override that existed to put it there has nothing left to do. `default_envs` keeps a bare `pio run`, `pio check` or `pio test` on the first of them.

A change that touches `lib/` or a task body is not verified by building it. Run the tests on the board, or say plainly that you did not.

## Conventions when editing

These are the invariants that are easiest to break silently. `ARCHITECTURE.md` explains why each one exists.

- **Adding a subsystem is four edits:** the task body in a new `src/*.cpp` reaching shared objects via `extern`, a `[[noreturn]] extern` declaration in `src/main.cpp`, its static storage (`StackType_t xStack[N]` and a `StaticTask_t`) beside it, and an `xTaskCreateStatic` in `setup()`. Tasks and queues are created nowhere else, and never with the dynamic `xTaskCreate` / `xQueueCreate` — CI greps `src/` for both and fails. Note `portable/FSP/port.c` contains its own `xTaskCreate` of 1024 words: it is unreachable only because no build here defines `AUTOSTART_FREERTOS` or `EARLY_AUTOSTART_FREERTOS`, and with the current heap it could not succeed. Do not define either.
- **Every queue carries its items by value, and nothing allocates at run time.** There is no heap-pointer protocol left to follow. `queue-mavlink-messages-by-value` moved the MAVLink queues off the heap once their items shrank below the point where by-value storage cost less than the machinery around it; `replace-messagepack-log-with-dataflash` moved the last one, `sdWriteQueue`, whose item is now the 32-byte `SdRecord` tagged union from `include/SdRecord.h`. Adding a queue means depth × item size in `.bss` and nothing else — **no producer/consumer margin**, because a by-value item held before a send or after a receive is a local on that task's own stack, not a shared block. `configTOTAL_HEAP_SIZE` is `0x0` and the linker drops the allocator from the image entirely, so a `pvPortMalloc` reintroduced anywhere fails on its first call and halts the board through the malloc-failed hook. **CI greps all of `src/` for it, textually** — so a file that needs to discuss the rule must describe it rather than name the function, or it fails its own check. One trap: `xPortGetFreeHeapSize()` still compiles and returns 0 for ever, which is why the housekeeping stream's `HeapFree`/`HeapMin` read 0. See `ARCHITECTURE.md` §4.
- **Stack sizes in `xTaskCreateStatic` are words, not bytes**, and are tuned tight (96–256). The count must match the length of the `StackType_t` array passed alongside it. After changing a task body, check that task's high-water mark before assuming it still fits — either in the SD log (`ARCHITECTURE.md` §5.2 now records the procedure, which needs the card pulled) or live off the `NAMED_VALUE_INT` housekeeping stream, which needs only the link.
- **Priorities come from `include/Priority.h`**, never raw numbers.
- **Only the owning file touches its resource:** `src/link.cpp` both MAVLink ports, `src/sdwrite.cpp` the card, `lib/SystemTime` the clocks, `lib/Battery` the ADC. Everything else goes through a queue or the library wrapper. This is what makes the absence of mutexes safe — do not break it by reaching for a peripheral directly.
- **Every outbound MAVLink message uses the same identity triple:** system id `1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.
- **Wiring is hardcoded** (SD `CS` on 9, battery on `A0`, DS1307 on I2C). If a change adds a pin, document it in `ARCHITECTURE.md`.
- **`configASSERT` in `setup()` halts only where recovery is impossible.** A missing RTC or SD card degrades instead: the board runs on ticks since boot and accepts a time set from the ground without the DS1307, and skips the housekeeping log without the card, reporting the absence either way — see `openspec/specs/fault-recovery/spec.md`. Queue and task creation still assert: with `configSUPPORT_STATIC_ALLOCATION` these cannot fail for want of memory, so a `NULL` handle there is a programming error, not a hardware fault. `VBTBKR[0..3]` belongs to the bootloader's double-tap magic and must never be written by this firmware; `include/Recovery.h` owns everything from `[4]` on.
- **The RAM budget is a build-time fact.** `scripts/ram_budget.py` runs after every link, prints the true commitment — which the `RAM:` line does not, omitting 9472 bytes — and fails the build when headroom drops below `custom_ram_min_headroom`. A change that does not fit fails on your desk, which is the point; do not lower the floor to make one pass.
- When a change makes `ARCHITECTURE.md` inaccurate, update it in the same commit.
