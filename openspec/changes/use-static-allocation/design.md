## Context

See `proposal.md` — Why. What follows is only what shapes the approach.

Facts established against the current tree before choosing it, because each of them
could have ruled the approach out or changed the numbers:

- **Every `FreeRTOSConfig.h` macro involved is `#ifndef`-guarded.** The whole kernel
  configuration is a project decision reachable from `build_flags`; nothing needs the
  PlatformIO package patched.
- **Turning dynamic allocation off is not possible.** `portable/MemMang/` contains
  exactly one file, `heap_4.c`, PlatformIO compiles it as part of
  `libArduino_FreeRTOS.a` unconditionally, and it opens with
  `#error This file must not be used if configSUPPORT_DYNAMIC_ALLOCATION is 0`.
  `pvPortMalloc` exists in this firmware whatever we do.
- **`src/main.cpp` is the only reachable creator of kernel objects, but it is not the
  only one that exists.** `portable/FSP/port.c` contains
  `_start_freertos_on_header_inclusion_impl()`, which creates a 1024-word "Sketch
  Thread" and starts the scheduler. It is linked in and unreachable only because the
  core provides weak no-op autostart hooks and this project defines neither
  `AUTOSTART_FREERTOS` nor `EARLY_AUTOSTART_FREERTOS`. Nothing under
  `.pio/libdeps/uno_r4_minima/` references `xTaskCreate`, `xQueueCreate` or
  `pvPortMalloc`, and TinyUSB's FreeRTOS abstraction is never compiled because
  `variants/MINIMA/tusb_config.h` sets `CFG_TUSB_OS` to `OPT_OS_NONE` explicitly. So
  converting `src/main.cpp` converts everything that runs — but with the heap at 6144
  that dormant path would need 4208 bytes it could no longer get, which makes the
  autostart macros a trap for whoever next adds a FreeRTOS-using library.
- **A trial link with `configSUPPORT_STATIC_ALLOCATION=1` asks for two symbols**,
  `vApplicationGetIdleTaskMemory` and `vApplicationGetTimerTaskMemory`. Adding
  `configUSE_TIMERS=0` reduces it to the first. The two flags travel together.

Three sizes set the arithmetic. `mavlink_message_t` is 291 bytes, which `heap_4`
stores as a 304-byte block. `Data` is 44 bytes — read out of the compiled
`logger.cpp.o`, where the allocation compiles to `movs r0, #44` — stored as 56.
`sizeof(StaticTask_t)` is 76 once the thread-local storage pointers are off, and
`sizeof(StaticQueue_t)` is 72.

## Goals / Non-Goals

**Goals:**

- Make the build refuse a firmware whose kernel objects do not fit, rather than
  producing one that dies after `vTaskStartScheduler()`, and make it say by how much.
- Make the RAM total `pio run` prints move when a task is added.
- Give every declared queue depth enough backing that saturation is reported by the
  queue rather than at the point of allocation.
- Keep this a relocation: identical steady-state behaviour, identical stack depths, so
  any regression is attributable to the move and nothing else.

**Non-Goals:**

- Removing `pvPortMalloc` from the message path. Queues keep carrying heap pointers;
  the by-value item is a separate, later change.
- Tuning stack sizes. That needs high-water marks, which needs a board.
- Fixing the four unchecked `pvPortMalloc` call sites, or the stack-overflow hook.
  Both have their own `TODO.md` entries and both should follow this change closely.
- Reclaiming `g_heap`. Its 8192 bytes are set by `BSP_CFG_HEAP_BYTES` in a generated
  variant header with no `#ifndef` guard, so it is not reachable from this project.

## Decisions

### Static and dynamic allocation coexist

`configSUPPORT_STATIC_ALLOCATION=1` is added; `configSUPPORT_DYNAMIC_ALLOCATION`
stays at 1. This is the ordinary FreeRTOS configuration, not a compromise — but it
does mean the guarantee is *"nothing in `src/` creates kernel objects dynamically"*
rather than *"it cannot"*.

**Alternative rejected:** patching `heap_4.c` or vendoring the kernel to drop the
`#error`. It moves a project decision into a package that PlatformIO may reinstall,
and it would have to be re-applied silently on every fresh checkout.

**Consequence handled here rather than deferred:** because every kernel-object
creation in `src/` is converted by this change, a grep for `xTaskCreate(` or
`xQueueCreate(` outside their `...Static` forms is a complete check today, and belongs
in CI. Landing it needs a token with the `workflow` scope, so it is planned here and
not yet done — tasks 5.3 and 5.4 stay open. That is a different question from `pvPortMalloc`,
which stays in `src/` on the message path and cannot be guarded until the by-value
item lands.

### The heap is sized from items in existence, not items in queues

The naive budget is `depth x block` per queue. It is wrong, and wrong in the direction
that matters: the protocol allocates *before* it sends, so for a producer to learn
from the queue that it is full, the block beyond the depth must already have been
obtained successfully. Consumers hold a block between `xQueueReceive` and `vPortFree`.
And three producers feed the outbound queue concurrently — `TaskHeartbeat`,
`TaskMavlinkBatteryStatus`, and `TaskMavlink`'s timesync reply.

| Queue | Depth | Also in existence | Blocks | Bytes |
|---|---|---|---|---|
| inbound link | 8 | 1 producer, 1 consumer | 10 x 304 | 3040 |
| outbound link | 4 | 3 producers, 1 consumer | 8 x 304 | 2432 |
| housekeeping | 4 | 1 producer, 1 consumer | 6 x 56 | 336 |
| | | | **Total** | **5808** |

`configTOTAL_HEAP_SIZE` becomes `0x1800` (6144). `heap_4` places its `pxEnd` block
inside the array, so 6136 are usable and the margin is 328 bytes.

The margin is deliberately more than a rounding allowance. With two block sizes
sharing one heap, coalescing limits fragmentation but does not eliminate it: a freed
304-byte block partly refilled by 56-byte blocks can leave the largest contiguous run
below 304 while the total free is well above it. 328 bytes of slack is not a proof
against that, and the requirement is written as an obligation on the reservation
rather than a claim about heap_4's behaviour.

**Alternative rejected:** `0x1000`, from the naive `depth x block` budget of 3872.
It leaves 216 bytes — less than one message block — and fails the very scenario the
spec requires.

**Alternative rejected:** keeping all three depths at 16 and backing them (over 12 KB).
It would consume the unclaimed RAM this change exists to expose, to buy slots that
measurement says are never used.

**Alternative rejected:** cutting the depths to 6/3/3 instead of raising the heap.
Recomputed with the in-flight allowance it still needs 4840 bytes, so the heap has to
grow either way; the depths are worth keeping.

The depths are asymmetric on purpose. The outbound drain vastly outruns its producers:
a `BATTERY_STATUS` frame is 48 bytes on the wire — `MAVLINK_MSG_ID_BATTERY_STATUS_LEN`
is 54 plus 12 non-payload bytes, and MAVLink 2 trims the trailing zero payload, which
here is the whole 18-byte extension block — so about 8.33 ms at 57600 baud, roughly
120 messages a second against the 2.5 the firmware emits. Four slots are already 33 ms
of buffer against producers that wake every 500 ms. The inbound queue instead faces a
burst when a ground station connects and hands its items to a `LOW`-priority consumer,
and dropping an inbound command is worse than dropping a telemetry frame that will be
resent within 500 ms.

### Stack depths are carried across verbatim

All seven keep their current word counts. Relocating and re-tuning in one change
would make any regression unattributable — which is precisely how the previous
attempt reached the board. Tuning follows once `add-console-cli` lands and `ps` can
report the high-water marks.

### The allocation-failure hook masks interrupts, signals, and never returns

`heap_4` calls `vApplicationMallocFailedHook()` after `xTaskResumeAll()`, in the
context of the task that asked for the memory, with the scheduler running and
interrupts enabled. That is the fact the design has to be built around.

A hook that signals and spins without masking interrupts does not stop the board: with
`configUSE_TIME_SLICING` at 0 and preemption on, it starves only tasks at or below the
failing task's priority. If `TaskLogger` at `LOW` is the one that fails,
`TaskSerialRead`, `TaskSerialWrite` and `TaskHeartbeat` keep running and keep
transmitting — a satellite that looks alive from the ground while it is out of memory.
Which tasks survive would depend on which one happened to allocate.

So the hook calls `taskDISABLE_INTERRUPTS()` first, then signals.

**Alternative rejected:** signalling and returning, so each caller's `NULL` check
handles it. Four call sites in `src/mavlink.cpp` do not check, so returning would
preserve the exact write-through-null that caused this incident.

**Correcting an earlier reading of the stack-overflow hook.** That hook's defect
(`TODO.md` — *The stack overflow hook hangs before it warns*) is not that it masks
interrupts. It is that having masked them it then waits on `Serial`, which can never
become ready, and calls `delay()`, which depends on a tick that has also stopped. A
register write to the indicator pin and a busy-wait on a `volatile` counter work
perfectly well with interrupts masked. Masking is the correct half of that hook; the
new one keeps it and avoids the rest.

**Accepted:** a halted board is an inert satellite, which is wrong in flight. It is
still strictly better than a silent death or a half-alive one, and the planned
watchdog and boot-recovery work turns this halt into a recorded reset.

### The overflow is made legible

ld's own diagnostic for this variant is not usable as a budget check. `fsp.ld` places
`.heap` and `.stack_dummy` at absolute addresses at the top of RAM, which defeats ld's
region accounting: a small overflow produces only a section-overlap message naming
`.heap` and two VMAs, with no size and no mention of the object that did not fit; a
large one adds `region RAM overflowed by 0 bytes`.

A PlatformIO `extra_scripts` post-build step therefore sums `.data`, `.bss`, `.heap`,
the main stack and the vector table against `RAM_LENGTH` and fails with the shortfall
in bytes. It is a dozen lines, it runs in CI, and it is what makes the spec's first
requirement literally true rather than approximately true.

### Ownership

No resource changes owner, and no new owner appears.

- **`src/main.cpp`** owns the static storage for the seven tasks and three queues,
  exactly as it owns their creation today. The composition-root rule is unchanged;
  the declarations sit beside the `xTaskCreateStatic` calls that consume them.
- **`src/hooks.cpp`** owns every kernel hook and, with them, the fault indicator on
  `LED_BUILTIN`. It already drives that pin from the stack-overflow hook, so the new
  allocation-failure hook belongs in the same file rather than becoming a second
  writer of the same pin; the two patterns are chosen together there and recorded in
  `ARCHITECTURE.md` §6 beside the resource table. `vApplicationGetIdleTaskMemory()`
  and the idle task's static storage go here too — kernel plumbing, not composition.
- The UART stays with `src/serial.cpp`, the card with `src/sdwrite.cpp`, the clocks
  with `lib/SystemTime`, the ADC with `lib/Battery`. This change reaches none of them.

### Two unused kernel facilities are switched off in the same change

The timer service — 824 bytes of heap (a 128-word task plus a 10-slot command queue of
12-byte items) and 140 bytes of `.bss` — and the five thread-local storage pointers per
task, 160 bytes across eight control blocks. Each was checked for callers: nothing
invokes `xTimerCreate`, and nothing in the port or the core reads or writes a
thread-local storage pointer.

`configQUEUE_REGISTRY_SIZE=0` is set as well, and buys nothing. `timers.c` is the only
caller of `vQueueAddToRegistry` in the whole tree, so once the timer service is off the
linker drops the 80-byte registry by itself; the 140-byte `.bss` figure above already
includes it. The flag stays to record the intent, not as a saving.

They belong here rather than in a separate change because turning the timer service
off is what removes `vApplicationGetTimerTaskMemory` from the link, and because both
are the same edit to the same block of `build_flags`.

## Risks / Trade-offs

- **The worst-case occupancy is derived from reading the producers and consumers, and
  the only test of it needs a board** → The margin is sized for that: 328 bytes over a
  budget that already counts every holder. A non-board task recomputes the occupancy
  from the code rather than from the queue depths, so the derivation is checked even
  though the behaviour cannot be.
- **Fragmentation is bounded by argument, not by proof** → Two block sizes, one heap,
  coalescing on free. Accepted, and the reason the margin is 328 rather than 8.
- **Nothing here can be verified on hardware right now** → `pio run` proves the budget
  fits and that no removed kernel facility is still referenced. That the scheduler
  runs, that no stack was silently under-sized, and that the hook behaves as designed
  all need the board. The tasks say which steps those are, and they stay unticked.
- **A halted board is an inert satellite** → Accepted; superseded by the planned
  watchdog and boot-recovery work. Until then it replaces a silent death with a
  visible one.
- **Stack overflow remains possible and its detector is broken** (`TODO.md` — *The
  stack overflow hook hangs before it warns*) → Not in scope, but it is the safety net
  for the stack depths this change deliberately does not touch. Sequence it next.
- **Reducing depths changes which failure path is reached** → At 2.5 messages a second
  against a drain of about 120, neither the old path nor the new one is reached in
  steady state. The change is which one a burst would find, and the new one is the one
  every producer already handles correctly.
- **The dormant autostart path in `port.c` would no longer fit** → It needs 4208 bytes
  and is unreachable because no build defines the autostart macros. Recorded in
  `CLAUDE.md` beside "tasks and queues are created nowhere else", because a future
  library that defines one of them would fail in a way nothing here would catch.
- **`bench` and `libs` inherit the flags** → Both must be built during implementation,
  since CI builds only the flight environment today; this change adds all three to CI.
  Note that `pio run -e libs` builds the application, not the Unity binary —
  `test_build_src` applies to `pio test` — so it carries the same static storage and
  fits by the same arithmetic. The Unity binary is the separate case, and it links no
  FreeRTOS objects at all.

## Migration Plan

There is no device state and no persisted format, so this is a plain code change.
Rollback is reverting the commit: the `build_flags` come back out, `xTaskCreate`
returns, and the firmware is byte-comparable to its predecessor.

The heap is resized before the tasks are converted, not after. Converting first leaves
an intermediate state with about 600 bytes of unclaimed RAM — it builds, so every
stated verification passes, but anything landing alongside it tips the build into the
opaque overlap error described above. Resizing first leaves every intermediate state
with kilobytes of slack.

The ordering that matters outside the change is that this lands first and
`add-console-cli` — the change that bricked the board — lands after it, because this
is what makes flashing it safe.

## Open Questions

- Whether the by-value queue change should later shrink `configTOTAL_HEAP_SIZE`
  further or leave it and take the win as `.bss` headroom. It cannot be answered until
  the item size is fixed, and it changes nothing here.
