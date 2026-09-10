## Why

The high-water marks exist to size the stacks, and today the only way to read them is
to power the board down and pull the SD card out. That closes the loop hours after the
change that needed checking, which is why `ARCHITECTURE.md` already reserves the USB
console for "diagnostics and a CLI" — it just has no owner yet.

Since the link moved to `Serial1`, the flight build leaves the USB CDC port open at
115200 and completely silent. A text CLI on that port reads the numbers off an
assembled, running board with no adapter, no GCS and no reflash, and it claims the
console owner that `ARCHITECTURE.md:317` says the first writer must claim.

## What Changes

- A new **console CLI**: a task in a new `src/cli.cpp` that owns the USB CDC port,
  reads newline-terminated commands and writes plain text replies. It is the port's
  first and only owner while tasks are running.
- Four commands, and nothing else:
  - `ps` — one row per FreeRTOS task: id, name, priority, state and stack words still
    free, followed by free heap.
  - `ps <name>` — the same table filtered to one task.
  - `free` — heap total, free now, and minimum ever free.
  - `help` / `?` — lists the commands.
- The task list comes from the kernel, not from a hand-maintained table, so a task
  added later appears without touching the CLI. This needs
  `-D configUSE_TRACE_FACILITY=1` and `-D INCLUDE_eTaskGetState=1` in
  `platformio.ini`, alongside the two `INCLUDE_*` flags already there.
- A new `include/Cli.h` naming the console port as `CLI_SERIAL`, defaulting to
  `Serial`, mirroring how `include/Link.h` names `LINK_SERIAL`. The `bench`
  environment — which moves the MAVLink link onto USB — overrides it to `Serial1`, so
  the two ports swap roles and no build has both protocols on one port.
- **`-D configUSE_TIMERS=0`.** Required, not optional: the CLI task does not fit in the
  heap otherwise. Nothing calls `xTimerCreate`, so the timer daemon and its command
  queue are 864 bytes spent on an unused feature. This removes the timer-service task
  from the running system, which is why `ps` is specified to report whatever the
  scheduler holds rather than a fixed roster.
- **The four unchecked `pvPortMalloc` results in `src/mavlink.cpp` are checked.** This
  change absorbs `TODO.md`'s *Check the result of `pvPortMalloc` in the four places
  that don't*, whose entry is deleted in the same commit. It is not a tidy-up: with the
  heap this tight, a failed allocation in a periodic sender writes a packed message to
  address `0`, and that is what took the board down when this change was first flashed.
- A HIL case in `test/test_hil/` driving the CLI over the console and asserting on the
  `ps` output.
- `ARCHITECTURE.md` gains the console's owner and the CLI's place in the task table;
  `CLAUDE.md`'s stale `pio device monitor` comment ("raw MAVLink bytes, not text") is
  corrected in the same change.

Not in scope, deliberately: a continuous `top`-style refresh, any command that changes
state, and publishing the same numbers over MAVLink — the latter stays as its own
`TODO.md` entry.

**No breaking changes.** The MAVLink surface is untouched: no new message id, no
changed stream rate, and the identity triple stays system id 1 /
`MAV_COMP_ID_AUTOPILOT1` / `MAV_TYPE_ROCKET`. What changes is that a port which
previously emitted nothing now answers text.

## Capabilities

### New Capabilities
- `console-cli`: the text command interface on the USB console — which port carries
  it, how a command is framed and dispatched, what `ps`, `free` and `help` report,
  and the guarantees the CLI owes the rest of the firmware (it must not block a task,
  must not become a second writer to any owned resource, and must not exist in a form
  that lets the ground change state).

### Modified Capabilities
- `mavlink-link`: its "Host attached to USB" scenario currently requires that a host
  opening the USB port "receives no text while the firmware is running normally; the
  console is reserved and its only writer is the stack-overflow handler". That
  reservation is now taken. The requirement that USB carries no *MAVLink frames* in
  the flight build is unchanged and must stay; only the console's silence changes.

## Impact

**RAM — this is the part that has to be justified.** Against the 8 KB FreeRTOS heap
(`configTOTAL_HEAP_SIZE` 0x2000), starting from the boot accounting in `TODO.md`'s
*The two serial queues cannot fit in the FreeRTOS heap*:

| Item | Cost | Where |
|---|---|---|
| Committed at boot before this change | 7312 B | heap |
| **Free before this change** | **880 B** | heap |
| CLI task at a provisional 192 words | 4 x 192 + 112 = **880 B** | heap |
| `configUSE_TRACE_FACILITY` adds `uxTCBNumber` + `uxTaskNumber` to every TCB | 8 B x 9 tasks = **72 B** | heap |
| **Required** | **952 B** | heap |
| Timer daemon and its command queue, reclaimed by `-D configUSE_TIMERS=0` | **+864 B** | heap |
| **Free after this change** | **792 B** | heap |
| `TaskStatus_t` snapshot buffer, 36 B x 12 slots | 432 B | `.bss`, **not** the heap |
| New queue | none | — |

**The change does not fit without reclaiming the timer daemon.** 952 bytes are needed
and 880 are free: a deficit of 72. Nothing in `src/`, `lib/` or the dependencies calls
`xTimerCreate`, so its task and command queue are 864 bytes paid for a feature the
firmware does not use, and `-D configUSE_TIMERS=0` returns them. That flag is
therefore part of this change, not an optimisation deferred to later — without it the
CLI task cannot be created, and because no `xTaskCreate` result is checked in
`src/main.cpp` the failure is silent and leaves the heap with nothing left for the
periodic senders.

This was found by flashing it. The first build of this change bricked the board: the
task creation failed, the heap went to zero, and a periodic sender wrote a packed
message to address `0`. Which is why this change also carries the `NULL` checks that
`TODO.md`'s *Check the result of `pvPortMalloc` in the four places that don't*
described — see What Changes.

Two notes on the table. The 192 words is a starting point, not a measurement: the
CLI formats text, which is what eats stack, and the figure has to be confirmed on the
board — `ps` reports its own row, so the change verifies its own headroom. And the
snapshot buffer is deliberately static rather than `pvPortMalloc`ed: the FreeRTOS heap
is itself a `.bss` array, so a static buffer costs the same 432 B of the 32 KB SRAM
without competing with queued messages for the 8 KB the tasks share.

**Flash.** This core has no `Serial.printf` — `Print.h` does not define it — so the
table is formatted with `print()` and padding loops rather than pulling `vfprintf`
out of newlib for one command.

**Files.** New: `src/cli.cpp`, `include/Cli.h`, a HIL case under `test/test_hil/`.
Modified: `src/main.cpp` (the `extern` declaration and the `xTaskCreate`),
`platformio.ini` (two build flags, and `CLI_SERIAL` in `bench`), `ARCHITECTURE.md`,
`CLAUDE.md`.

**Backlog.** No `TODO.md` entry is deleted: this work was scoped directly and never
had one. But `TODO.md`'s *"Debug and release builds, with MAVLink tracing on the
console"* competes for this exact port and leaves open the same four questions this
change has to answer anyway — whether a console write can block a task, how
concurrent writers interleave, what formatting costs in stack, and what it costs in
flash. That entry must be re-pointed at this change rather than left to drift, since
after this the console has an owner and a trace cannot simply `print` into it.

**Related defects, not fixed here.** `src/mavlink.cpp`'s `sendStatusText` builds its
message with pointer arithmetic on a string literal, and several outbound packers do
not check `pvPortMalloc` against `NULL`. The first already has a `TODO.md` entry that
notes number formatting "should be solved once and not in two places" — the CLI is
now a third place, so the two should be read together.
