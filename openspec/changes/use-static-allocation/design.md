## Context

See `proposal.md` — Why. What follows is only what shapes the approach.

Three facts about this framework were established against the current tree before
choosing it, because each of them could have ruled the approach out:

- **Every `FreeRTOSConfig.h` macro involved is `#ifndef`-guarded.** The whole kernel
  configuration is a project decision reachable from `build_flags`; nothing needs the
  PlatformIO package patched.
- **Turning dynamic allocation off is not possible.** `portable/MemMang/` contains
  exactly one file, `heap_4.c`, PlatformIO compiles it as part of
  `libArduino_FreeRTOS.a` unconditionally, and it opens with
  `#error This file must not be used if configSUPPORT_DYNAMIC_ALLOCATION is 0`.
  `pvPortMalloc` exists in this firmware whatever we do.
- **Nothing outside `src/` creates kernel objects.** TinyUSB is built with
  `CFG_TUSB_OS` at its `OPT_OS_NONE` default, so its FreeRTOS abstraction is never
  compiled, and no library under `.pio/libdeps/uno_r4_minima/` references
  `xTaskCreate`, `xQueueCreate` or `pvPortMalloc`. `src/main.cpp` really is the only
  creator, so converting it converts everything.

A trial link with `configSUPPORT_STATIC_ALLOCATION=1` against the current tree asks
for exactly one symbol: `vApplicationGetIdleTaskMemory`.

Two item sizes set the arithmetic. `mavlink_message_t` is 291 bytes, which `heap_4`
stores as a 304-byte block. `Data` is 44 bytes — read out of the compiled
`logger.cpp.o`, where the allocation compiles to `movs r0, #44` — which stores as 56.

## Goals / Non-Goals

**Goals:**

- Make the build refuse a firmware whose kernel objects do not fit, rather than
  producing one that dies after `vTaskStartScheduler()`.
- Make the RAM figure `pio run` prints move when a task is added.
- Give every declared queue depth enough backing that saturation is reported by the
  queue rather than by the allocator.
- Keep this a relocation: identical behaviour, identical stack depths, so any
  regression is attributable to the move and nothing else.

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

**Consequence to accept:** the property is enforced by review today. Once the
by-value queue change removes the last `pvPortMalloc` from `src/`, a grep in CI can
enforce it mechanically. Not yet — this change deliberately leaves the message path
alone, so `src/` still allocates.

### The heap is sized from the depths, not the other way round

| Queue | Depth | Item block | Heap required |
|---|---|---|---|
| inbound link | 8 | 304 B | 2432 B |
| outbound link | 4 | 304 B | 1216 B |
| housekeeping | 4 | 56 B | 224 B |
| | | **Total** | **3872 B** |

`configTOTAL_HEAP_SIZE` becomes `0x1000` (4096), leaving 224 bytes of margin. Every
queue can be full at once and every allocation still succeeds, which is what the
"declared depth is backed" requirement asks for.

The depths are asymmetric on purpose. The outbound drain is roughly 34x faster than
its producers — a 68-byte `BATTERY_STATUS` occupies the link for about 11.8 ms at
57600 baud, so about 85 messages a second against the 2.5 the firmware emits — and 4
slots are already 47 ms of buffer against producers that wake every 500 ms. The
inbound queue instead faces a burst when a ground station connects and hands its
items to a `LOW`-priority consumer, and dropping an inbound command is worse than
dropping a telemetry frame that will be resent within 500 ms.

**Alternative rejected:** keeping all three at 16 and raising the heap to back them
(9728 bytes). It would consume the unclaimed RAM this change is trying to expose as
policed headroom, to buy slots that measurement says are never used.

**Alternative rejected:** keeping the depths at 16 unbacked, as today. That is the
status quo whose failure mode is a null pointer in four unchecked places.

### Stack depths are carried across verbatim

All seven keep their current word counts. Relocating and re-tuning in one change
would make any regression unattributable — which is precisely how the previous
attempt reached the board. Tuning follows once `add-console-cli` lands and `ps` can
report the high-water marks.

### The allocation-failure indication signals and halts, and depends on nothing

`vApplicationMallocFailedHook()` is called by `heap_4` at the point of failure,
before `NULL` reaches the caller. It signals and does not return.

**Alternative rejected:** signalling and returning, so each caller's `NULL` check
handles it. Four call sites in `src/mavlink.cpp` do not check, so returning would
preserve the exact write-through-null that caused this incident.

The indication must not repeat the defect the stack-overflow hook has: that hook
calls `taskDISABLE_INTERRUPTS()` and then waits on `Serial`, which can never become
ready once the USB interrupt is masked. The new hook depends on no interrupt, no
tick and no host — direct pin manipulation and a busy-wait.

**Accepted for now:** halting leaves the satellite inert, which is wrong in flight.
It is still strictly better than today's silent death, and the planned watchdog and
boot-recovery work turns this halt into a recorded reset. Recorded in Risks.

### Ownership

No resource changes owner, and no new owner appears.

- **`src/main.cpp`** owns the static storage for the seven tasks and three queues,
  exactly as it owns their creation today. The composition-root rule is unchanged;
  the declarations sit beside the `xTaskCreateStatic` calls that consume them.
- **`src/hooks.cpp`** owns every kernel hook and, with them, the fault indicator. It
  already drives `LED_BUILTIN` from the stack-overflow hook, so the new
  allocation-failure hook belongs in the same file rather than becoming a second
  writer of the same pin. `vApplicationGetIdleTaskMemory()` and the idle task's
  static storage go here too — it is kernel plumbing, not composition.
- The UART stays with `src/serial.cpp`, the card with `src/sdwrite.cpp`, the clocks
  with `lib/SystemTime`, the ADC with `lib/Battery`. This change reaches none of them.

### Four unused kernel facilities are switched off in the same change

The timer service (864 B), the five thread-local storage pointers per task (160 B
across eight control blocks), and the queue registry (80 B, measured as `0x50` in the
current map). Each was checked for callers: nothing invokes `xTimerCreate`, nothing
reads or writes a thread-local storage pointer anywhere in the port or the core, and
the only `vQueueAddToRegistry` in the tree is in TinyUSB's FreeRTOS abstraction, which
is not compiled.

They belong here rather than in a separate change because turning the timer service
off is what removes the need for `vApplicationGetTimerTaskMemory`, and because all
three are the same edit to the same block of `build_flags`.

## Risks / Trade-offs

- **The size estimates for `StaticTask_t` and `StaticQueue_t` come from the kernel
  headers, not from the board** → The first build is the check. If they are wrong the
  link fails and reports by how much, which is the behaviour this change exists to
  create. Low consequence by construction.
- **Nothing here can be verified on hardware right now** → `pio run` proves the budget
  fits and that no removed kernel facility is still referenced. That the scheduler
  runs, that no stack was silently under-sized, and that the new hook signals
  correctly all need the board. Tasks say which steps those are, and they stay unticked
  until someone can flash it.
- **A halted board is an inert satellite** → Accepted; superseded by the planned
  watchdog and boot-recovery work. Until then it replaces a silent death with a
  visible one.
- **Stack overflow remains possible and its detector is broken** (`TODO.md` — *The
  stack overflow hook hangs before it warns*) → Not in scope, but it is the safety net
  for the stack depths this change deliberately does not touch. Sequence it next.
- **Reducing depths changes which failure path is reached** → At 2.5 messages a second
  against a drain of about 85, neither the old path nor the new one is reached in
  steady state. The change is which one a burst would find, and the new one is the one
  every producer already handles correctly.
- **`bench` and `libs` inherit the flags** → `libs` builds with `test_build_src` off,
  so it creates no tasks and needs no static storage; a 4096-byte heap is ample for a
  binary that creates nothing. Both should be built during implementation to confirm
  it, since neither is exercised by CI.

## Migration Plan

There is no device state and no persisted format, so this is a plain code change.
Rollback is reverting the commit: the `build_flags` come back out, `xTaskCreate`
returns, and the firmware is byte-comparable to its predecessor.

The ordering that matters is around it rather than inside it. This change lands
first; `add-console-cli` — the change that bricked the board — lands after it, because
this is what makes flashing it safe.

## Open Questions

- Whether the by-value queue change should later shrink `configTOTAL_HEAP_SIZE`
  further or leave it and take the win as `.bss` headroom. It cannot be answered until
  the item size is fixed, and it changes nothing here.
