## Why

The firmware's RAM budget is invisible to the linker, so nothing can refuse a
build that does not fit. Every task stack, TCB and queue is carved out of
`ucHeap[8192]` at runtime; to the linker that is one anonymous array. `pio run`
prints `RAM: 51.4%` (16828 of 32768) after every build — a figure that excludes
both heaps and does not move by a single byte when a task is added.

The real figure is 7312 of 8192 bytes committed at boot and **~870 free**. That
gap between the number on screen and the number on the board is what let the
`add-console-cli` change reach the hardware: it was 72 bytes short, `xTaskCreate`
consumed the last of the heap, an unchecked `pvPortMalloc` returned `NULL`, and the
board has been unreachable since — USB CDC is serviced from an interrupt, and both
plausible death paths mask it, so the 1200-baud touch into the bootloader no longer
works. Recovery needs physical access.

The backlog makes this structural rather than a one-off. The entries already written
in `TODO.md` — IMU, temperature, charge detection, `SYS_STATUS`, `NAMED_VALUE_*`,
MAVLink FTP, the console CLI, the second MAVLink endpoint on USB — would roughly
double the task count. At `4 x stack_words + 112` bytes each, that is around 7 KB
against 870 available.

Measured against the current `.elf`, the RAM is there; it is partitioned so that
none of it can be reached:

| Region | Bytes | Who can use it |
|---|---|---|
| `.data` + `.noinit` + `.bss` less `ucHeap` | 8636 | statics and library buffers |
| `ucHeap` (FreeRTOS) | 8192 | tasks, queues, queued items — ~870 free |
| `g_heap` (newlib `malloc`) | 8192 | `JsonDocument`, `String`, SD |
| **Unclaimed by any section** | **6464** | **nobody** |
| `g_main_stack` + vector table | 1280 | — |

This change does not add capacity. It moves the kernel objects into `.bss`, where
the linker counts them by name, so that a firmware which does not fit fails on the
developer's desk instead of on the board.

## What Changes

- **Tasks and queues are created statically.** `xTaskCreateStatic` and
  `xQueueCreateStatic` replace `xTaskCreate` and `xQueueCreate` in `src/main.cpp`,
  with the stack array, `StaticTask_t` and queue storage declared beside them. The
  composition root stays the only place that creates anything.
- **The FreeRTOS heap is resized to what it actually backs.**
  `configTOTAL_HEAP_SIZE` drops from `0x2000` to `0x1000`, sized so that every
  declared queue depth can be satisfied at once.
- **Queue depths become reachable.** 16 / 16 / 16 today are fiction: the heap runs
  out after two or three messages, so a queue never fills and the allocator always
  fails first. They become 8 (`serialReadQueue`), 4 (`serialWriteQueue`) and 4
  (`sdWriteQueue`), fully backed by the resized heap. **BREAKING** for the
  observable failure mode: under a burst, a producer now sees `xQueueSend` return
  other than `pdPASS` — the path every producer already handles — instead of a
  `NULL` allocation.
- **A failed allocation is reported.** `configUSE_MALLOC_FAILED_HOOK` is enabled and
  `vApplicationMallocFailedHook()` signals it. The failure that bricked the board was
  silent.
- **Four kernel features that nothing uses are switched off**: the timer daemon
  (864 B — nothing calls `xTimerCreate`), the thread-local storage pointers (160 B
  across eight TCBs), the queue registry (80 B), and with them the last reason for
  `vApplicationGetTimerTaskMemory` to exist.
- **`vApplicationGetIdleTaskMemory()` is added** to `src/hooks.cpp`. A link with
  `configSUPPORT_STATIC_ALLOCATION=1` was tried against the current tree and this is
  the only symbol it asks for.
- **Stack sizes are not touched.** All seven keep their current word counts, so this
  change is a pure relocation and any regression stays attributable. Tuning them
  needs the high-water marks, which needs a board.
- `ARCHITECTURE.md` §3 and §4 and the `CLAUDE.md` conventions are updated in the same
  commit, and the `TODO.md` entry *The two serial queues cannot fit in the FreeRTOS
  heap* is deleted, its two inbound cross-references re-pointed at this change.

Not changed: the MAVLink surface. No message id, stream rate or identity field moves,
and the queue item protocol — heap pointers, never values — stays exactly as it is.

## Capabilities

### New Capabilities
- `memory-budget`: where the firmware's RAM commitments live, which of them the
  linker is required to account for, what backs a declared queue depth, and how an
  allocation failure is surfaced.

### Modified Capabilities
<!-- None. `mavlink-link` describes which port carries the link and at what rate;
     nothing in it changes. -->

## Impact

- **`platformio.ini`** — six `build_flags` added to `[env:uno_r4_minima]`, inherited
  by `bench` and `libs`. Every `FreeRTOSConfig.h` macro involved is `#ifndef`-guarded,
  so none of this needs the framework package patched.
- **`src/main.cpp`** — static storage for seven tasks and three queues, and the
  `...Static` creation calls. Grows by roughly 25 lines, which become the readable
  RAM budget.
- **`src/hooks.cpp`** — `vApplicationGetIdleTaskMemory()` and
  `vApplicationMallocFailedHook()`.
- **`ARCHITECTURE.md`, `CLAUDE.md`, `TODO.md`** — the invariants that this change
  makes inaccurate.
- **RAM**, computed from the kernel headers rather than measured on the board: about
  6100 bytes of stacks, TCBs and queue structures move from `ucHeap` into `.bss`;
  `ucHeap` drops by 4096; `.bss` grows by roughly 1800 net. The unclaimed gap falls
  from 6464 to about 4700 bytes, and every byte of it becomes headroom the linker
  polices. Heap left for queued items rises from ~870 to ~4096. The first build is
  the verification: if the arithmetic is wrong, the link fails and says by how much.
- **Not verifiable in CI, and not verifiable without the board.** `pio run` proves
  the budget fits and that nothing was left referring to a removed kernel feature.
  That the scheduler still runs, that no stack was silently under-sized by the move,
  and that the malloc-failed hook signals correctly all need hardware that is
  currently unreachable.
- **Blocked by nothing; blocks little.** `add-console-cli` should land after this, not
  before — this is what makes it safe to flash. The by-value queue entry
  (`TODO.md`) and the periodic-schedule collapse both become cheaper afterwards, and
  neither is in scope here.
