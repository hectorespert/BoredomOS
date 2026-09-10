Steps marked **[board]** cannot be done while the board is unreachable. Everything
else is verified by `pio run`, which is also all CI runs.

## 1. Turn on static allocation and give the idle task its memory

- [ ] 1.1 Add `-D configSUPPORT_STATIC_ALLOCATION=1` and `-D configUSE_TIMERS=0` to
  `build_flags` in `[env:uno_r4_minima]`, and verify `pio run` fails at the link with
  exactly one undefined reference, `vApplicationGetIdleTaskMemory` — confirming on
  this tree that nothing else the kernel needs is missing
- [ ] 1.2 Add `vApplicationGetIdleTaskMemory()` to `src/hooks.cpp`, returning a
  file-scope `StaticTask_t` and a `StackType_t` array of `configMINIMAL_STACK_SIZE`
  words, and verify `pio run` now links
- [ ] 1.3 Add `-D configNUM_THREAD_LOCAL_STORAGE_POINTERS=0` and
  `-D configQUEUE_REGISTRY_SIZE=0`, and verify `pio run` succeeds and
  `arm-none-eabi-nm` on the `.elf` no longer reports an `xQueueRegistry` symbol

## 2. Make an allocation failure visible

- [ ] 2.1 Add `-D configUSE_MALLOC_FAILED_HOOK=1` and verify `pio run` fails at the
  link asking for `vApplicationMallocFailedHook`
- [ ] 2.2 Implement `vApplicationMallocFailedHook()` in `src/hooks.cpp` so it signals
  on `LED_BUILTIN` by direct pin manipulation and a busy-wait, depends on no
  interrupt, no tick and no attached host, and does not return; verify `pio run`
  links and that the function contains no call to `delay()`, `Serial` or
  `taskDISABLE_INTERRUPTS()`
- [ ] 2.3 Give it a blink pattern distinguishable from the stack-overflow hook's, and
  record both patterns in `ARCHITECTURE.md` so the two faults can be told apart from
  outside the board

## 3. Reserve the task memory statically

- [ ] 3.1 Declare, in `src/main.cpp` beside the existing handles, a `StackType_t`
  array and a `StaticTask_t` for each of the seven tasks, and verify every array
  length is character-for-character the word count that task passes to `xTaskCreate`
  today — 96, 192, 128, 256, 96, 128, 256
- [ ] 3.2 Replace the seven `xTaskCreate` calls with `xTaskCreateStatic`, passing the
  new storage and leaving name, depth, parameter and priority untouched, and verify
  `pio run` succeeds
- [ ] 3.3 `configASSERT` each returned handle is non-`NULL`, and verify `pio run`
  still succeeds — with static storage this can only fail on a bad argument, which is
  a programming error worth trapping at boot

## 4. Reserve the queue memory statically and size the heap to it

- [ ] 4.1 Declare a `StaticQueue_t` and an item-storage array for each of the three
  queues, at depths 8 (inbound link), 4 (outbound link) and 4 (housekeeping), and
  verify each storage array is `depth * sizeof(item pointer)` bytes
- [ ] 4.2 Replace the three `xQueueCreate` calls with `xQueueCreateStatic`, keeping
  the existing `configASSERT` on each handle, and verify `pio run` succeeds
- [ ] 4.3 Set `-D configTOTAL_HEAP_SIZE=0x1000` and verify `pio run` succeeds — this
  is the number the design derives as 3872 bytes of backing plus 224 of margin
- [ ] 4.4 Verify with `arm-none-eabi-nm -S` that every task stack, task control block
  and queue structure now appears in `.bss` under its own symbol, and that `ucHeap` is
  4096 bytes

## 5. Confirm the budget is now policed by the build

- [ ] 5.1 Temporarily declare an eighth task whose stack cannot fit in the RAM that
  remains, and verify the link fails and names the overflowing region and its size —
  this is the change's headline requirement and it needs no hardware; remove the task
  afterwards and verify `pio run` succeeds again
- [ ] 5.2 Verify the RAM figure `pio run` prints has increased from 16828 bytes and
  now moves when a task is added or removed, and record the new figure in
  `ARCHITECTURE.md`
- [ ] 5.3 Verify `pio run -e bench` and `pio run -e libs` both succeed, since both
  inherit these flags and neither is built by CI

## 6. Update the documents the change makes inaccurate

- [ ] 6.1 Update `ARCHITECTURE.md` §3 so "adding a subsystem is three edits" reflects
  the static storage declaration, and verify the section names every edit an
  implementer must make
- [ ] 6.2 Update `ARCHITECTURE.md` §4 to state what backs each queue depth and that a
  saturated queue is now reported by `xQueueSend` rather than by a failed allocation,
  and add the RAM table from the proposal so the budget is stated where the design is
- [ ] 6.3 Update the `CLAUDE.md` conventions: stack sizes are still words but now name
  a declared array, tasks and queues are created statically, and the heap backs queued
  items only
- [ ] 6.4 Delete the `TODO.md` entry *The two serial queues cannot fit in the FreeRTOS
  heap*, and re-point the two entries that cross-reference it — *Check the result of
  `pvPortMalloc` in the four places that don't* and *Queue the message intent by value
  instead of a packed `mavlink_message_t`* — at this change id, verifying no
  `*[The two serial queues...]*` reference is left dangling

## 7. Verify on the board — blocked until the board is recoverable

- [ ] 7.1 **[board]** Flash with `pio run -t upload` and verify the scheduler starts
  and all seven tasks reach their steady-state cadence
- [ ] 7.2 **[board]** Run `pio test` and verify all nine HIL cases pass, confirming the
  message set, rates and identity are unchanged by the move
- [ ] 7.3 **[board]** Read the high-water marks from the SD housekeeping log and verify
  no task's margin has narrowed relative to a pre-change log — this is the check that
  the relocation under-sized nothing
- [ ] 7.4 **[board]** Force an allocation failure with a temporary oversized
  `pvPortMalloc` and verify the board signals with the expected pattern, with no host
  attached, and does not return; remove it afterwards
- [ ] 7.5 **[board]** Verify a saturated queue is reported by `xQueueSend` and not by a
  failed allocation, by temporarily stalling a consumer and watching the producer's
  path
