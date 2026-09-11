## Context

See `proposal.md` — Why. The facts that shape the approach, all verified against the
tree and the installed toolchain:

- The flight build leaves the USB CDC port open and unused. The Renesas core's own
  `main()` calls `Serial.begin(115200)` before `setup()`, and since the link moved to
  `Serial1` nothing writes to it. `ARCHITECTURE.md:261` records it as **no owner**.
- Every knob in the port's `FreeRTOSConfig.h` is `#ifndef`-guarded, so
  `configUSE_TRACE_FACILITY` can be turned on from `build_flags` exactly like the two
  `INCLUDE_*` flags already in `platformio.ini`. That one flag is the whole cost:
  `INCLUDE_eTaskGetState` is not required, because `uxTaskGetSystemState` derives
  `eCurrentState` from the list each task is queued on rather than calling
  `eTaskGetState`.
- `configUSE_TIMERS` is 0 since `use-static-allocation` and `configUSE_MUTEXES` is 0. So the timer-service task
  exists today and appears in `ps`, and there is no priority inheritance — a task has
  one priority, not a base and a current one. `TODO.md`'s *[The two serial queues
  cannot fit in the FreeRTOS heap]* proposes `-D configUSE_TIMERS=0` to reclaim
  864 bytes, since nothing calls `xTimerCreate`; the spec therefore requires whatever
  the scheduler reports rather than a fixed count, and this design must not encode
  one either.
- `configMAX_TASK_NAME_LEN` is 16, so the kernel stores at most 15 characters of a
  name. `MavlinkBatteryStatus` is already truncated inside FreeRTOS today.
- The core's `Print` class has **no `printf`**. Formatting is `print()` plus padding,
  or `snprintf` and its newlib cost.
- The heap is `heap_4`, so `xPortGetMinimumEverFreeHeapSize()` is available and `free`
  can report a low-water mark, not just the instantaneous figure.

## Goals / Non-Goals

**Goals:**

- Give the console port an owner, and make that ownership the mechanism by which any
  future console writer — starting with the MAVLink trace in the backlog — reaches it.
- Read the numbers off a running board with no adapter, no GCS and no reflash.
- Survive a task being added later without anyone remembering to edit the CLI.

**Non-Goals:**

- A command framework. There are four commands; a dispatch table arrives when the
  fifth does.
- CPU usage per task. It needs `configGENERATE_RUN_TIME_STATS` and a run-time counter
  the Renesas port does not wire up, and the watermark answers the question the stacks
  actually pose.
- Line editing, history, backspace handling beyond what a terminal sends, or escape
  sequences. The console is a diagnostic, not a shell.

## Decisions

### One port each, rather than two protocols on one port

The reference implementation for this kind of CLI — madflight's — sniffs the first
bytes of the stream and switches between a text mode and a MAVLink mode, never
returning to text once a frame is seen. That complexity exists because its board has
one port and three protocols.

Here the flight build already has a free port, so the CLI takes USB and MAVLink keeps
`Serial1`. No sniffing, no mode state machine, no ambiguity about whether a byte is a
command or a frame header.

The one collision is the `bench` environment, which moves the link onto USB so the HIL
suite needs no adapter. Rather than compiling the CLI out there, `include/Cli.h`
defines `CLI_SERIAL` defaulting to `Serial`, mirroring `include/Link.h`, and `bench`
overrides it to `Serial1`. The two ports swap roles, both builds contain both
features, and there is no `#ifdef`-ed-out code path that stops being compiled and
quietly rots.

*Alternative rejected:* `-D CLI_ENABLED=0` in `bench`. It halves what the bench build
proves about the flight build, and dead code that CI never compiles is how a change
breaks a configuration nobody flashed that week.

### The task list comes from the kernel, not from a table

`configUSE_TRACE_FACILITY=1` unlocks `uxTaskGetSystemState()`, which fills a caller-
provided array with one `TaskStatus_t` per task, including idle and timer-service.

*Alternative rejected:* iterating a hand-written array of the seven `TaskHandle_t`
globals in `src/main.cpp`, which costs nothing to enable. It was rejected on
maintenance, not on cost: `ARCHITECTURE.md` promises that adding a subsystem is three
edits, and a static table makes it four, with a failure mode — the new task silently
missing from `ps` — that nobody notices until they need the number.

The cost of the kernel route is 8 bytes added to every TCB (`uxTCBNumber` and
`uxTaskNumber`), which is heap. See `proposal.md` — Impact for the full accounting.

### The snapshot buffer is static, and there is only one of it

`TaskStatus_t` is 36 bytes on this target. The array is sized for 12 tasks (9 exist
today: seven ours, idle, timer service) and lives in `.bss`, not on the CLI's stack —
432 bytes would not fit in a 192-word stack — and not on the FreeRTOS heap. The
FreeRTOS heap is itself a `.bss` array, so a static buffer costs the same SRAM without
competing with queued messages for the 8 KB the tasks share, and it cannot fail at the
moment it is needed.

madflight keeps **two** snapshots and diffs them to derive CPU%. With CPU% out of
scope, one snapshot is enough — half the RAM, and none of the O(n²) matching of task
numbers between generations.

If more than 12 tasks ever exist, `uxTaskGetSystemState()` returns 0 rather than
overflowing. `ps` must report that as an explicit error rather than printing an empty
table, so the failure is legible instead of looking like a working board.

### Formatting without `printf`

Columns are aligned with `print()` and a padding loop — the same approach madflight
uses for its own help text — rather than pulling `vfprintf` out of newlib for one
command. That keeps the flash cost proportional to the feature and avoids the trap
that the same backlog entry already flags: number formatting existing in several
places at once.

### Resource ownership

| Resource | Owner after this change |
|---|---|
| USB CDC console (`CLI_SERIAL`) | **`src/cli.cpp`** — new, and the port's first owner |
| MAVLink link UART (`LINK_SERIAL`) | `src/serial.cpp`, unchanged |
| SD card | `src/sdwrite.cpp`, unchanged |
| RTC / clocks | `lib/SystemTime`, unchanged |
| Battery ADC | `lib/Battery`, unchanged |

`src/cli.cpp` reads task and heap figures through FreeRTOS API calls, which are
lock-free reads and not a second owner of anything. It does not touch the card, the
ADC, the clocks or the link. It creates no queue: it is the only writer to its port,
so it writes directly and needs none.

The one pre-existing exception stays an exception: `src/hooks.cpp` writes to the same
port from the stack-overflow handler. That runs with interrupts disabled after the
scheduler has stopped, so it cannot interleave with a CLI reply. It is documented as
such rather than routed through the CLI, because the CLI may be exactly what overflowed.

### Priority and cadence

The CLI task runs at `PRIORITY_LOWEST`. It polls its port and sleeps, in the shape
`TaskSerialRead` already uses. Nothing on the console is urgent, and the spec's
requirement that the CLI never delay a flight task is easiest to honour by making it
the lowest thing that runs.

## Risks / Trade-offs

- **A blocking write stalls the task.** `Serial.write` on USB CDC can block when the
  host stops draining. At `PRIORITY_LOWEST` that starves nothing, but the CLI task
  itself can park indefinitely mid-reply. → Acceptable at this priority, and it is
  bounded: the CLI holds no lock and owns nothing another task needs. It must not be
  "fixed" by raising the priority.
- **Formatting eats stack, and stacks here are tight.** → The 192-word figure is
  provisional. `ps` reports its own row, so the first thing the change verifies is its
  own headroom, and `configCHECK_FOR_STACK_OVERFLOW=2` is already on.
- **~960 bytes of an 8 KB heap, ~12%.** → Real, and the reason the snapshot buffer was
  kept out of the heap. The measurement to take before calling this done is
  `free`'s minimum-ever figure with the CLI in place, not the build succeeding.
- **The trace facility is on in the flight build.** → It costs 8 bytes per TCB and
  changes no behaviour, but it is a kernel configuration change reaching every task,
  so the high-water marks are re-read after enabling it, not assumed unchanged.
- **A second console writer appears later.** The backlog's MAVLink trace wants this
  port. → This change makes that a design question with an owner to answer it rather
  than a free-for-all; `proposal.md` — Impact records that the entry must be
  re-pointed here.
- **`bench` moves the CLI to `Serial1`, where the HIL suite cannot reach it.** →
  Handled in `tasks.md`; it is the one place where the two-port swap costs something.

## Open Questions

- Whether `ps` should print a column for each task's configured stack size. The kernel
  cannot supply it — `pxEndOfStack` exists only where the stack grows upward, and on
  Cortex-M it grows down — so it would mean a hand-maintained table, which is the
  thing this design rejected. Left out for now; the reader compares against
  `src/main.cpp`.

### Resolved while writing the task list

**Which port the HIL case opens.** `test/test_hil/check_silence.py` already solves
this shape: it finds the USB CDC device by vendor id, independently of the link, and
raises `NoLinkError` — reported as `IGNORE`, not `FAIL` — when the build has put the
link on USB. The CLI case is its mirror image: it opens the console port the same way
and reports `IGNORE` when the CLI is not reachable there, which is exactly the `bench`
case where `CLI_SERIAL` has been swapped to `Serial1`. So `pio test` keeps passing
under `bench` with the CLI case ignored, and running the suite against the flight
build exercises it. No new convention, and no environment variable beyond the existing
`HIL_PORT`.

**A consequence for the existing suite.** `check_silence.py` asserts the USB console
carries no text. That stays true only because the CLI emits nothing unsolicited — no
boot banner, no greeting. This is a design commitment, not an accident: a banner would
turn a passing check into a failing one and would break the spec requirement that the
console is silent until spoken to.
