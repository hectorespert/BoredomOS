CI runs `pio run` and `pio check`, and nothing else. Every step below is verified by
one of those unless it is marked **[board]**, which means it cannot be done while the
board is unreachable.

## 1. Turn on static allocation, give the idle task its memory, and resize the heap

- [x] 1.1 Add `-D configSUPPORT_STATIC_ALLOCATION=1` to `build_flags` in
  `[env:uno_r4_minima]` and verify `pio run` fails at the link asking for **two**
  symbols, `vApplicationGetIdleTaskMemory` and `vApplicationGetTimerTaskMemory`; then
  add `-D configUSE_TIMERS=0` and verify only the first remains — confirming on this
  tree that the timer flag is what removes the second and that nothing else the kernel
  needs is missing. The tree is knowingly unbuildable until 1.2.
- [x] 1.2 Add `vApplicationGetIdleTaskMemory()` to `src/hooks.cpp`, returning a
  file-scope `StaticTask_t` and a `StackType_t` array of `configMINIMAL_STACK_SIZE`
  words, and verify `pio run` now links
- [x] 1.3 Add `-D configNUM_THREAD_LOCAL_STORAGE_POINTERS=0` and
  `-D configQUEUE_REGISTRY_SIZE=0`, and verify with `arm-none-eabi-nm` that the `.elf`
  contains no `xQueueRegistry`, no `xTimerQueue` and no `pxCurrentTimerList` — the
  registry disappears because `timers.c` was its only caller, so this confirms both
  facilities are gone rather than one
- [x] 1.4 Set `-D configTOTAL_HEAP_SIZE=0x1800` and verify `pio run` succeeds and
  `arm-none-eabi-nm -S` reports `ucHeap` at 6144 bytes. This precedes the conversion on purpose: doing it
  afterwards leaves an intermediate with about 600 bytes of unclaimed RAM, where any
  overflow surfaces as the opaque section-overlap error of task 5.2. The intermediate
  produced here is not runnable — 6144 bytes cannot back seven tasks still created
  dynamically — which is acceptable only because nothing is flashed until group 7.

## 2. Make an allocation failure stop the board visibly

- [x] 2.1 Add `-D configUSE_MALLOC_FAILED_HOOK=1` and verify `pio run` fails at the
  link asking for `vApplicationMallocFailedHook`
- [x] 2.2 Implement `vApplicationMallocFailedHook()` in `src/hooks.cpp` so it calls
  `taskDISABLE_INTERRUPTS()` first and then signals on `LED_BUILTIN` by direct pin
  manipulation and a busy-wait on a `volatile` counter, and never returns. Verify
  `pio run` links and that the function body contains no `Serial`, no `delay()`, and
  no wait on any condition that an interrupt or the scheduler tick would have to
  satisfy. Masking interrupts is required, not forbidden: `heap_4` calls this hook
  with the scheduler running, so signalling without masking would leave
  higher-priority tasks transmitting from a firmware that is out of memory.
- [x] 2.3 Choose a blink pattern distinguishable from the stack-overflow hook's, and
  record both patterns in `ARCHITECTURE.md` §6 beside the resource table row for the
  status LED, verifying the two faults can be told apart by an observer with nothing
  attached

## 3. Reserve the task memory statically

- [x] 3.1 Declare, in `src/main.cpp` beside the existing handles, a `StackType_t`
  array and a `StaticTask_t` for each of the seven tasks that are created, and verify
  every array length is the word count that task passes to `xTaskCreate` today — 96,
  192, 128, 256, 96, 128, 256. Note that `src/main.cpp` also declares an eighth
  `extern` task, `TaskSensors`, which is defined nowhere and created nowhere; it gets
  no storage.
- [x] 3.2 Replace the seven `xTaskCreate` calls with `xTaskCreateStatic`, passing the
  new storage and leaving name, depth, parameter and priority untouched, and verify
  `pio run` succeeds
- [x] 3.3 `configASSERT` each returned handle is non-`NULL`, and verify `pio run`
  still succeeds — with static storage this can only fail on a bad argument, which is
  a programming error worth trapping at boot

## 4. Reserve the queue memory statically

- [x] 4.1 Declare a `StaticQueue_t` and an item-storage array for each of the three
  queues, at depths 8 (inbound link), 4 (outbound link) and 4 (housekeeping), and
  verify each storage array is `depth * sizeof(item pointer)` bytes — 32, 16 and 16
- [x] 4.2 Replace the three `xQueueCreate` calls with `xQueueCreateStatic`, keeping
  the existing `configASSERT` on each handle, and verify `pio run` succeeds
- [x] 4.3 Recompute the worst-case heap occupancy from the code rather than from the
  queue depths — for each queue, its depth plus one block per producer that can be
  holding an unsent item and one per consumer that can be holding an unreleased one —
  and verify the total is the 5808 bytes the design budgets against 6136 usable. This
  is the only check the derivation gets; the behaviour itself needs the board.
- [x] 4.4 Verify with `arm-none-eabi-nm -S` that every task stack, task control block
  and queue structure now appears in `.bss` under its own symbol

## 5. Make the budget legible and keep it policed

- [x] 5.1 Add a PlatformIO `extra_scripts` post-build step that sums `.data`, `.bss`,
  `.heap`, the main stack and the vector table against `RAM_LENGTH` from
  `memory_regions.ld`, prints the true commitment and the headroom on every build, and
  fails with a non-zero result and the shortfall in bytes when the headroom falls below
  a declared minimum. Verify it passes on the current tree, prints the expected
  figures, and fails with the right shortfall when the minimum is temporarily raised
  above the actual headroom. Note what it is and is not for: because `fsp.ld` places
  `.heap` and `.stack_dummy` at absolute addresses, an actual overflow already fails
  the link — but as a section-overlap message naming two addresses, no size and not the
  object that did not fit, and a post-build step never runs after a failed link. This
  check exists to fail *before* that point, while the numbers are still legible.
- [x] 5.2 Temporarily add an eighth task — declared, given static storage, **and
  created with `xTaskCreateStatic`** — whose stack cannot fit in the RAM that remains,
  and verify the build fails. Confirm both halves of the behaviour: with the headroom
  minimum in place the post-build check fails first and names the shortfall in bytes;
  with it lowered, the link itself fails with the bare section-overlap message, which
  is the diagnostic 5.1 exists to pre-empt. Declaring the storage without creating the
  task is not sufficient:
  `--gc-sections` and dead-store elimination remove an unused array and the build goes
  green at an unchanged RAM figure. Remove the task afterwards and verify `pio run`
  succeeds again.
- [x] 5.3 Add a CI step that greps `src/` for `xTaskCreate(` and `xQueueCreate(`
  outside their `...Static` forms and fails if either is found, and verify it passes
  on the converted tree and fails on a reverted line. This is a complete check today
  because this change converts every kernel-object creation in `src/`; it says nothing
  about `pvPortMalloc`, which stays on the message path.
- [x] 5.4 Change CI from a bare `pio run` to `pio run -e uno_r4_minima -e bench -e libs`
  and verify all three build, since `default_envs` otherwise pins CI to the flight
  environment and leaves the new budget policed in one configuration of three
- [x] 5.5 Verify `pio test -e libs --without-uploading --without-testing` still builds
  the Unity binary — it links no FreeRTOS objects at all, so it is unaffected, but it
  is the case `pio run -e libs` does not cover
- [x] 5.6 Verify the RAM figure `pio run` prints has risen from 16828 bytes to roughly
  20650, and record in `ARCHITECTURE.md` §8 both that figure and the ~2650 bytes of
  unclaimed RAM behind it — the printed total still omits `g_heap`, the main stack and
  the vector table, so on its own it understates the commitment by 9472 bytes
- [x] 5.7 Verify `openspec validate use-static-allocation --strict` and `pio check`
  are both clean, with no new defects against the 12 pre-existing LOW ones

## 6. Update the documents the change makes inaccurate

- [x] 6.1 Update `ARCHITECTURE.md` §3 so "adding a subsystem is three edits" reflects
  the static storage declaration, and verify the section names every edit an
  implementer must make
- [x] 6.2 Update `ARCHITECTURE.md` §4 to state what backs each queue depth, including
  the in-flight allowance, and that a saturated queue is now reported by `xQueueSend`
  rather than at the point of allocation
- [x] 6.3 Update `ARCHITECTURE.md` §7, which states the 8 KB heap and the
  `4 x stack_words + 112` cost as the constraints that shape the code, and add task
  creation to its list of what `configASSERT` traps in `setup()`
- [x] 6.4 Update the `CLAUDE.md` conventions: stack sizes are still words but now name
  a declared array, tasks and queues are created statically, the heap backs queued
  items only, and no build may define `AUTOSTART_FREERTOS` or
  `EARLY_AUTOSTART_FREERTOS` — the core's own task creator in `port.c` needs 4208
  bytes it can no longer get
- [x] 6.5 Update the `context` block and the `rules.proposal` entry in
  `openspec/config.yaml`, which state the 8 KB heap, the `4 x stack_words + 112` cost
  and the three-edit pattern as hard constraints. This is the highest-value document
  in the group: its text is injected into every future proposal, and it is the model
  `add-console-cli` sized itself against.
- [x] 6.6 Delete the `TODO.md` entry *The two serial queues cannot fit in the FreeRTOS
  heap*, and re-point the two entries that cross-reference it — *Check the result of
  `pvPortMalloc` in the four places that don't* and *Queue the message intent by value
  instead of a packed `mavlink_message_t`* — at this change id, verifying no reference
  to the deleted entry is left dangling
- [x] 6.7 Add a `TODO.md` entry for recovering the board, so the blocked steps in group
  7 point at something that is tracked, and reference it from this change
- [x] 6.8 Re-point the two unapplied changes at the static model: `add-console-cli`
  creates its task with `xTaskCreate`, claims `-D configUSE_TIMERS=0` for itself, and
  justifies its RAM against the 8 KB heap and a 72-byte deficit that no longer exists;
  `add-usb-dual-protocol` grows the console stack from 192 to 384 words and deletes the
  `bench` environment that task 5.4 adds to CI. Verify neither still asserts something
  this change contradicts.

## 7. Verify on the board — blocked until the board is recoverable

- [ ] 7.1 **[board]** Flash with `pio run -t upload` and verify the scheduler starts
  and all seven tasks reach their steady-state cadence
- [ ] 7.2 **[board]** Run `pio test` and verify all nine HIL cases pass, confirming the
  message set, rates and identity are unchanged by the move. Without a board every
  case reports `IGNORE` and the command exits 0, so a green run off the board proves
  nothing.
- [ ] 7.3 **[board]** Read the high-water marks from the SD housekeeping log and verify
  no task's margin has narrowed relative to a pre-change log — this is the check that
  the relocation under-sized nothing. The free-heap field in the same record will have
  dropped by about 2 KB; that is the resized heap, not a regression.
- [ ] 7.4 **[board]** Force an allocation failure with a temporary oversized
  `pvPortMalloc` and verify the board signals with the expected pattern, with no host
  attached, and that telemetry stops — not merely that the indicator lights; remove it
  afterwards
- [ ] 7.5 **[board]** Verify a saturated queue is reported by `xQueueSend` and not by a
  failed allocation, by temporarily stalling a consumer and watching the producer's
  path
