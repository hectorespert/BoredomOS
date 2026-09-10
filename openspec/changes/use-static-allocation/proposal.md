## Why

The firmware's RAM budget is invisible to the linker, so nothing can refuse a
build that does not fit. Every task stack, TCB and queue is carved out of
`ucHeap[8192]` at runtime; to the linker that is one anonymous array.

`pio run` prints `RAM: 51.4%` (16828 of 32768) after every build. The figure is
honest about `ucHeap` — the array lives in `.bss` and is counted in full — and that
is precisely why it is useless: the eight kilobytes are counted whether they hold
seven tasks or none, so **adding a task moves the number by zero**. What it does not
count is `g_heap`, the main stack and the vector table, another 9472 bytes.

The number that matters is inside that array: 7272 of 8192 bytes committed at boot,
**912 free**. That gap between the figure on screen and the state of the board is
what let the `add-console-cli` change reach the hardware: it was 72 bytes short,
`xTaskCreate` consumed the last of the heap, an unchecked `pvPortMalloc` returned
`NULL`, and the board has been unreachable since — USB CDC is serviced from an
interrupt, and both plausible death paths mask it, so the 1200-baud touch into the
bootloader no longer works. Recovery needs physical access.

The backlog makes this structural rather than a one-off. The entries already written
in `TODO.md` — IMU, temperature, charge detection, `SYS_STATUS`, `NAMED_VALUE_*`,
MAVLink FTP, the console CLI, the second MAVLink endpoint on USB — would roughly
double the task count. At `4 x stack_words + 112` bytes each, that is around 7 KB
against 912 available.

Measured against the current `.elf`, the RAM is there; it is partitioned so that
none of it can be reached:

| Region | Bytes | Who can use it |
|---|---|---|
| `.data` + `.noinit` + `.bss` less `ucHeap` | 8636 | statics and library buffers |
| `ucHeap` (FreeRTOS) | 8192 | tasks, queues, queued items — 912 free |
| `g_heap` (newlib `malloc`) | 8192 | `JsonDocument`, `String`, SD |
| **Unclaimed by any section** | **6468** | **nobody** |
| `g_main_stack` + vector table | 1280 | — |

This change does not add capacity. It moves the kernel objects into `.bss`, where
the linker counts them by name, so that a firmware which does not fit fails on the
developer's desk instead of on the board.

## What Changes

- **Tasks and queues are created statically.** `xTaskCreateStatic` and
  `xQueueCreateStatic` replace `xTaskCreate` and `xQueueCreate` in `src/main.cpp`,
  with the stack array, `StaticTask_t` and queue storage declared beside them. The
  composition root stays the only place that creates anything.
- **The FreeRTOS heap is resized to what it must actually back.**
  `configTOTAL_HEAP_SIZE` goes from `0x2000` to `0x1800`, sized from the worst-case
  number of items *in existence* — not the number sitting in queues. A producer
  allocates before it sends, so learning "the queue is full" costs one block beyond
  the depth; consumers hold a block between receive and free; and three producers
  feed the outbound queue concurrently. The budget is 5808 bytes against 6136 usable.
- **Queue depths become reachable.** 16 / 16 / 16 today are fiction: the heap runs
  out after two or three messages, so a queue never fills and the allocator always
  fails first. They become 8 (inbound link), 4 (outbound link) and 4 (housekeeping),
  fully backed. **BREAKING** for the observable failure mode under saturation: a
  producer now sees `xQueueSend` return other than `pdPASS` — the path every producer
  already handles — instead of a `NULL` allocation. Steady-state behaviour is
  unchanged.
- **A failed allocation is reported and the board stops.** `configUSE_MALLOC_FAILED_HOOK`
  is enabled and `vApplicationMallocFailedHook()` signals the condition, masks
  interrupts and does not return. The hook runs in the failing task's context with the
  scheduler still running, so signalling without stopping the scheduler would leave
  higher-priority tasks emitting telemetry from a firmware that has run out of memory.
  The failure that bricked the board was silent; a half-silent one would be worse.
- **Two kernel features that nothing uses are switched off**: the timer service —
  824 bytes of heap and 140 of `.bss`, and nothing calls `xTimerCreate` — and the
  thread-local storage pointers, 160 bytes across eight control blocks. Turning the
  timer service off also removes the need for `vApplicationGetTimerTaskMemory` and,
  because `timers.c` is the only caller of `vQueueAddToRegistry`, lets the linker drop
  the queue registry on its own. `configQUEUE_REGISTRY_SIZE=0` is set to record the
  intent, and buys no further bytes.
- **`vApplicationGetIdleTaskMemory()` is added** to `src/hooks.cpp`. A link with
  `configSUPPORT_STATIC_ALLOCATION=1` alone asks for two symbols; once the timer
  service is off it asks for this one.
- **Stack sizes are not touched.** All seven keep their current word counts, so this
  change is a pure relocation and any regression stays attributable. Tuning them
  needs the high-water marks, which needs a board.
- **The budget is made legible when it overflows.** The linker's own diagnostic for
  this variant is a section-overlap message that names no size, because `fsp.ld`
  places `.heap` and `.stack_dummy` at absolute addresses and defeats ld's region
  accounting. A post-build check sums the RAM sections against `RAM_LENGTH` and
  reports the shortfall in bytes.
- `ARCHITECTURE.md` §3, §4, §6, §7 and §8, the `CLAUDE.md` conventions and the
  `openspec/config.yaml` project context are updated in the same commit, and the
  `TODO.md` entry *The two serial queues cannot fit in the FreeRTOS heap* is deleted,
  its two inbound cross-references re-pointed at this change.

Not changed: the MAVLink surface. No message id, stream rate or identity field moves,
and the queue item protocol — heap pointers, never values — stays exactly as it is.

## Capabilities

### New Capabilities
- `memory-budget`: where the firmware's RAM commitments live, which of them the
  linker is required to account for, what backs a declared queue depth, and how an
  allocation failure is surfaced.

### Modified Capabilities

None. `mavlink-link` describes which port carries the link, at what speed and what
may be assumed about it being ready; nothing in it changes.

## Impact

- **`platformio.ini`** — five `build_flags` added to `[env:uno_r4_minima]`, inherited
  by `bench` and `libs`, plus an `extra_scripts` entry for the RAM check. Every
  `FreeRTOSConfig.h` macro involved is `#ifndef`-guarded, so none of this needs the
  framework package patched.
- **`src/main.cpp`** — static storage for seven tasks and three queues, and the
  `...Static` creation calls. Grows by roughly 25 lines, which become the readable
  RAM budget.
- **`src/hooks.cpp`** — `vApplicationGetIdleTaskMemory()` and
  `vApplicationMallocFailedHook()`.
- **`ARCHITECTURE.md`, `CLAUDE.md`, `TODO.md`, `openspec/config.yaml`** — the
  invariants this change makes inaccurate. `config.yaml` matters most of the four: its
  project context is injected into every future proposal and currently states the
  8 KB heap and the `4 x stack_words + 112` cost as hard constraints. That model is
  what `add-console-cli` sized itself against.
- **RAM**: 6008 bytes of stacks, control blocks and queue structures move from
  `ucHeap` into `.bss` (4608 of stacks, 532 of control blocks, 588 for the idle task,
  216 of queue structures, 64 of item storage); `ucHeap` drops by 2048; the timer
  service returns 140 bytes of `.bss`. Net `.bss` growth about 3820, taking the figure
  `pio run` prints to roughly 20650 of 32768, and the unclaimed gap from 6468 to about
  2650 — every byte of which becomes headroom the linker polices. Heap left for queued
  items rises from 912 to 6136. Figures are computed from the kernel headers and
  confirmed against a trial build; the first real build is the check.
- **Not verifiable in CI, and not verifiable without the board.** `pio run` proves the
  budget fits and that nothing still refers to a removed kernel feature. That the
  scheduler runs, that no stack was silently under-sized by the move, and that the
  allocation-failure hook behaves as designed all need hardware that is currently
  unreachable — see the `TODO.md` entry for recovering the board.
- **Follow-up edits this forces on unapplied changes.** `add-console-cli` creates its
  task with `xTaskCreate`, claims `-D configUSE_TIMERS=0` for itself, and justifies
  its RAM against the 8 KB heap and a 72-byte deficit that will no longer exist;
  `add-usb-dual-protocol` grows the console stack from 192 to 384 words and deletes
  the `bench` environment. Both need re-pointing at the static model.
