## Why

The firmware has **1468 bytes of RAM headroom** against the 1024-byte floor
`custom_ram_min_headroom` sets — 444 bytes of slack. `add-usb-dual-protocol`
estimates it needs 768 of them to grow the console task's stack, which would leave
700 and fail `scripts/ram_budget.py` on the desk of whoever builds it. Something has
to give before that change can land, and the cheapest thing to give is a task nobody
needs.

Three tasks produce periodic telemetry, and two of them do nothing else.
`TaskHeartbeat` and `TaskMavlinkBatteryStatus` wake on `vTaskDelayUntil`, pack a
message, post it to `serialWriteQueue` and sleep again — each paying a full stack and
a full control block for a loop that is one `if` and one function call. `TaskMavlink`
already runs a loop against the same queue set, in the same file, and already blocks
with a timeout available to it. Folding the two producers into it costs no new task
and no new kernel feature.

## What Changes

- **`TaskHeartbeat` and `TaskMavlinkBatteryStatus` stop existing as tasks.** Their
  bodies — `sendHeartbeat()`, `sendSystemTime()`, `sendBatteryStatus()` — are
  unchanged and stay in `src/mavlink.cpp`; what disappears is the two `for(;;)`
  wrappers, their stacks, their control blocks and their handles.
- **`TaskMavlink` gains a `{function, interval_ms, last_ms}` schedule** and emits
  everything due on each pass. Everything due, not one message per pass: this feeds a
  queue rather than a port with a finite transmit buffer, so there is nothing to
  ration and the cadence stays exact.
- **`TaskMavlink`'s `xQueueReceive` stops using `portMAX_DELAY`** and waits instead
  until the next scheduled emission, which is what lets one task both consume inbound
  frames and keep a clock.
- **`TaskMavlink` moves from `PRIORITY_LOW` to `PRIORITY_HIGH`**, so telemetry
  production keeps the band it has today rather than dropping to the band that
  `ARCHITECTURE.md` §3 reserves for work nothing waits on. The alternative — leaving
  the task at `LOW` and letting telemetry fall with it — is rejected in `design.md`.
- **The recovery mechanisms move with the heartbeat.** Clearing the consecutive-boot
  counter after the stability window and firing the 30-minute retry out of the reduced
  configuration currently live in `TaskHeartbeat` because, as the comment at
  `src/mavlink.cpp:196` records, it is the only task that runs in every configuration.
  `TaskMavlink` also starts in the reduced configuration, so the property those
  mechanisms depend on survives the move intact.
- **The reduced configuration stops being expressed by not creating a task.**
  `BATTERY_STATUS` is withheld today by skipping `xTaskCreateStatic`; afterwards the
  schedule omits that entry. The observable result is the same and the storage is
  reclaimed in both configurations rather than sitting unused in the reduced one.
- **The housekeeping log loses two fields.** `heartbeatAvailableStack` and
  `statusAvailableStack` describe tasks that no longer exist.
- **`MAVLINK_COMM_NUM_BUFFERS` drops from its default of 4 to 1.** The library
  reserves a `mavlink_message_t` and a `mavlink_status_t` per channel whether or not a
  channel exists; this firmware parses one, `MAVLINK_COMM_0` at `src/serial.cpp:41`.
  It is unrelated to the fold and is included because it is the same question —
  storage reserved for something that is not there — answered by one build flag in the
  same commit.

**This does not alter the MAVLink surface.** No new message id, no new stream rate, no
changed identity triple. The same three messages leave at the same three cadences —
`HEARTBEAT` and `SYSTEM_TIME` at 1 Hz, `BATTERY_STATUS` every 2 s — with system id
`1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET` and `MAV_AUTOPILOT_GENERIC`. A ground
station cannot tell this change happened, which is the property that makes it
verifiable: the HIL suite passing unchanged *is* the evidence.

Whether `SYSTEM_TIME` deserves 1 Hz is a real question and deliberately not asked
here. Retuning a rate is a change to the MAVLink surface with a spec delta and HIL
consequences, and mixing it into a refactor would mean a failing case could not
distinguish the two. It goes to `TODO.md`, where the schedule table this change
introduces makes it a one-number edit.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

None. Every requirement in `mavlink-link` survives this change word for word: the
message set, the three cadences, the identity triple and the "no task waits for the
link port" rule are all properties of what leaves the link, and none of them names a
task. `memory-budget` is likewise untouched — it requires that the build account for
each stack and control block individually and fail when headroom runs out, which is
the mechanism this change exercises rather than modifies, and its scenario "the RAM
total it reports increases by at least that task's stack and control block" holds in
reverse here. `console-cli` already requires `ps` to report whatever the scheduler
knows rather than a fixed roster, so two fewer rows need no requirement change.

This change therefore sets `skip_specs: true` in `.openspec.yaml`. It is a pure
refactor: behaviour does not change, so no spec should.

## Impact

**RAM.** Measured, not quoted. `pio run` cannot report it on this host —
`$SIZETOOL` is an x86_64 binary and the machine is arm64, so the `ram_budget.py`
post-action cannot execute — so the figures below come from parsing the section
headers of `.pio/build/uno_r4_minima/firmware.elf` directly, using exactly the
sections `RAM_SECTIONS` names, against `RAM_LENGTH = 0x8000` from the MINIMA
variant's `memory_regions.ld`. CI reports the same numbers the ordinary way.

| | Bytes |
|---|---|
| `.data` 740 + `.noinit` 28 + `.bss` 21060 + `.heap` 8192 + `.stack_dummy` 1024 + `.vector_table` 256 | |
| committed today | 31300 of 32768 |
| **headroom today** | **1468** (floor 1024) |

What this change returns, with `sizeof(StaticTask_t)` read as `0x54` = 84 bytes from
`firmware.map` — the 76 bytes `use-static-allocation/design.md` records predates
`-D configUSE_TRACE_FACILITY=1`, which adds 8:

| Removed | Stack | Control block | Total |
|---|---|---|---|
| `TaskHeartbeat` | 128 w = 512 B | 84 B | 596 B |
| `TaskMavlinkBatteryStatus` | 128 w = 512 B | 84 B | 596 B |
| | | **freed** | **1192 B** |

And from the channel buffers, with both figures read from the same image's symbol
table rather than derived from the library's headers:

| Removed | Today | With one channel | Freed |
|---|---|---|---|
| `m_mavlink_buffer` (4 × `mavlink_message_t`) | 1164 B | 291 B | 873 B |
| `m_mavlink_status`, two instantiations of 4 × 24 B | 192 B | 48 B | up to 144 B |

Headroom becomes **at least 3533 bytes** — 1468 + 1192 + 873 — and up to 3677 if both
status copies collapse. After `add-usb-dual-protocol` spends its 768 and gives 315
back by raising the flag to 2 for its second parser state, it still leaves over 2400,
against 700 and failing without this change.

The 1468 is CI's own figure, from the last green run on `master` — `.bss 21060`,
`committed 31300 B of 32768 (95.5%)`, `headroom 1468 B (minimum 1024)` — not a local
build: `arm-none-eabi-g++` in `toolchain-gccarmnoneeabi` is x86_64 and macOS 27
uninstalls Rosetta during the upgrade, so nothing compiles on an Apple Silicon host
until it is reinstalled. **While that holds, every `pio run` verification in
`tasks.md` is read from the CI log** (`gh run view <id> --log | grep -A4 "RAM
budget"`), which is the only place `scripts/ram_budget.py` actually executes.

The savings above are derived from symbol sizes in that same image rather than from a
build with the change applied, which does not exist yet. That is why tasks 3.4 and 3.6
verify the result against the map file and the CI report instead of trusting the
arithmetic.

No task, queue or library is added, so there is no new heap commitment and no queue
depth to back. `serialWriteQueue` keeps its depth of 4, but **its producer count
falls from three tasks to one**: every `xQueueSend` against it already lives in
`src/mavlink.cpp`, and afterwards all of them execute in `TaskMavlink`. The
arithmetic in `ARCHITECTURE.md` §4 — depth 4, plus one block per producer holding an
unsent item, plus one for the consumer — therefore needs 6 blocks where it reserves
8, and the heap's committed total falls from 5808 to 5200 of 6136 usable.

Nothing shrinks as a result. `configTOTAL_HEAP_SIZE` stays at `0x1800`, because
lowering it returns RAM only by removing the margin that surplus represents, and
re-deriving this heap downwards is how this project bricked a board once. The gain is
robustness, not bytes: the margin against the worst case grows from 328 to 936 bytes.
`design.md` records the re-derivation so the next change to a queue depth starts from
the true figure.

`TaskMavlink`'s stack stays at 256 words. The three producer bodies fit in 128 words
today and do not nest with the inbound dispatch, so the existing allocation should
cover the union — but "should" is why the task list measures the high-water mark on
the board before this is called done.

**Files.** `src/mavlink.cpp` (two task bodies become a schedule inside a third),
`src/main.cpp` (two `StackType_t` arrays, two `StaticTask_t`, two handles, two
`extern` declarations and two `xTaskCreateStatic` calls removed; `TaskMavlink`'s
priority changed), `src/logger.cpp` (two `extern TaskHandle_t`, two sampled fields),
`include/Data.h` (`Tasks` goes from seven fields to five), `src/sdwrite.cpp` (two
`JsonDocument` fields), `platformio.ini` (`-D MAVLINK_COMM_NUM_BUFFERS=1`),
`ARCHITECTURE.md` (the task table, the priority rationale in §3, the `portMAX_DELAY`
claim in §3, §4's producer count and §5.1), `TODO.md` (the entries that name the two
deleted tasks).

**Log schema.** Records written after this change carry five per-task high-water
marks where earlier ones carry seven. `lib/SdData`'s ring appends into files that may
already hold the old shape, so a card can end up with both. Nothing in the firmware
reads a record back, so this costs nothing in flight; whoever reads the `.mpk` files
on the ground has to tolerate two shapes. `mavlinkAvailableStack` also changes
meaning: it now covers the inbound dispatch *and* all three producers, which makes it
the number that matters most in the log.

**What this invalidates in `add-usb-dual-protocol`**, the only other active change:

- `design.md:106-107` states "`TaskHeartbeat` and `TaskMavlinkBatteryStatus` produce
  for the queue that feeds `Serial1`. They are left alone." After this change there
  are no such tasks to leave alone. The sentence needs rewriting around the schedule
  inside `TaskMavlink`; what it was asserting — that the radio path is untouched by
  the USB work — remains true.
- Task 2.2 asks to "confirm the high-water marks of both producer tasks are
  unchanged". There will be one producer, and its mark will have changed for reasons
  belonging to this change.
- Task 2.3 asks to "reduce `TaskMavlink` to receive / dispatch / queue-the-reply /
  free". This change moves it the other way. The two are compatible — that task's
  real content is extracting the builders behind `include/Mavlink.h`, which this
  change neither does nor blocks — but the wording assumes a task that will no longer
  exist in that shape.
- Its RAM Impact table measures its 768 bytes against a headroom this change moves.
  The cost does not change; the conclusion about whether it fits does.
- **It now shares a build flag with this change.** Its second parser state is
  `MAVLINK_COMM_1`, which does not exist once `MAVLINK_COMM_NUM_BUFFERS` is 1. That
  change must raise the flag to 2 alongside adding the state, and carry the 315 bytes
  it costs in its own table. Two active changes claiming one build flag is the exact
  failure `openspec/config.yaml` records as having happened once without anything
  detecting it, so it is stated here and again in task 7.3.

**Ordering.** This change is expected to land first. Nothing forces it to —
`add-usb-dual-protocol` is buildable in either order — but built second it fails
`ram_budget.py`, which is the whole reason this change exists.
