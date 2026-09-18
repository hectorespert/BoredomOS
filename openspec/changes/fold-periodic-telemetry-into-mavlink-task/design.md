## Context

See `proposal.md` — Why. What matters here is the shape being changed and the four
constraints that rule out the obvious alternatives.

Today `src/mavlink.cpp` holds three task bodies. Two are `vTaskDelayUntil` loops that
pack a message and post it; the third blocks on `xQueueReceive(serialReadQueue, ...,
portMAX_DELAY)` and dispatches inbound frames. Every `xQueueSend` against
`serialWriteQueue` in the firmware is already in this one file — six of them, at lines
61, 82, 102, 168, 247 and 351 — split across the three tasks only by which loop
happens to run them.

The constraints that shape the approach:

- **`configUSE_TIMERS` is 0**, set by `use-static-allocation` to reclaim the timer
  service. Software timers are not available without reversing that.
- **`configUSE_TIME_SLICING` was 0** when the decisions below were first written —
  the framework's own default, never a choice this project had made. Tasks of equal
  priority did not preempt each other; a task that never blocked kept the CPU until
  it did. Revised during this same change's review to `1`; see Decisions.
- **Static allocation only.** Every task's storage is declared in `src/main.cpp`
  whether or not the task is started, so an unstarted task costs exactly as much RAM
  as a started one.
- **One owner per resource.** This is what makes the absence of mutexes safe.

## Goals / Non-Goals

**Goals:**

- Reclaim the two tasks' stacks and control blocks without adding anything.
- Leave the link observably identical, so "the HIL suite still passes" is proof.
- Keep every resource with exactly one owning file.
- Leave a schedule that makes a future rate change a one-line edit.

**Non-Goals:**

- Retuning any rate. `SYSTEM_TIME` at 1 Hz is questioned in `TODO.md`, not here.
- Folding `TaskLogger` in as well. It is a different pipeline at a different priority
  feeding a different queue, and the priority collapse that would force is the thing
  this design spends its one priority change avoiding.
- Shrinking `configTOTAL_HEAP_SIZE` or any queue depth.
- Anything on the USB port. That is `add-usb-dual-protocol`.

## Decisions

### The schedule goes inside `TaskMavlink`, not into a new task and not into timers

Three shapes were costed against the 1192 bytes this change returns.

| Shape | Frees | Costs | Net |
|---|---|---|---|
| Merge the two producers into one new task | 596 B | — | **596 B** |
| Two software timers | 1192 B | ~970 B | **~220 B** |
| Schedule inside `TaskMavlink` | 1192 B | — | **1192 B** |

The timer cost is the timer service coming back: a daemon task (128 words plus an
84-byte control block), the command queue at `configTIMER_QUEUE_LENGTH`, the timer
lists and a `StaticTimer_t` per timer — close to the ~964 bytes
`use-static-allocation` measured when it switched the service off. Timers only start
to pay here at three or more periodic producers, and the third is `TaskLogger`, whose
inclusion means every callback collapses onto the single `configTIMER_TASK_PRIORITY`.
That is the priority separation `ARCHITECTURE.md` §3 exists to state.

Worth recording because it is the usual objection and it does not apply: a timer
callback must not block, and every `xQueueSend` here already passes a timeout of `0`.
The rejection is arithmetic and priority, not safety.

### `TaskMavlink` moves to `PRIORITY_HIGH`

One of the two priority assignments in `ARCHITECTURE.md` §3 has to move: telemetry
production is `HIGH`, protocol handling is `LOW`, and after the fold they are the same
task.

Raising the task preserves the guarantee that is written down as a requirement —
`mavlink-link`'s "Link traffic does not disturb periodic cadences", whose scenario
names `HEARTBEAT` and `SYSTEM_TIME` leaving at 1 Hz. Lowering telemetry to `LOW`
would put it level with `TaskLogger` and, with time slicing off, make both dependent
on the other yielding.

It also preserves §3's stated reason for `TaskSerialWrite` sitting at `HIGH` rather
than `HIGHEST`: "the writer sits at `HIGH`, level with its own producers, which yield
every cycle". After this change `TaskMavlink` *is* its only producer, at `HIGH` —
but it does not actually yield every cycle, only when `serialReadQueue` is empty. A
sustained inbound stream keeps `xQueueReceive` returning without blocking, which
could starve `TaskSerialWrite` at equal priority. Caught in Copilot's review of this
PR, not written down here first; see the next Decision for the fix.

*(Lowering telemetry to `LOW` instead, mentioned above as putting it "level with
`TaskLogger` and... dependent on the other yielding" — that consequence was specific
to time slicing being off. It no longer applies once the next Decision turns it on,
but the choice to raise rather than lower stands on the `mavlink-link` requirement
regardless.)*

What moves up with it: `mavlink_msg_*_decode`, `systemTime.setUnixTime()` and the
`default` case's `sendStatusText`. All bounded, and all previously ran above
`TaskSdWrite` anyway. The one to watch is `setUnixTime()`, which writes the DS1307
over I2C — see Risks.

### `configUSE_TIME_SLICING` moves from the framework's default of `0` to `1`

The decision above raises `TaskMavlink` to `PRIORITY_HIGH`, sharing that band with
`TaskSerialWrite`. Copilot's review of this change's PR pointed out what that
combination means with time slicing off: `xQueueReceive` only yields the CPU when it
genuinely blocks, and returns at once whenever `serialReadQueue` already holds a
frame. A sustained inbound stream could therefore let `TaskMavlink` run indefinitely
without ever giving `TaskSerialWrite` a turn, backing `serialWriteQueue` up until
outbound telemetry is dropped. This risk did not exist before this change:
`TaskMavlink` sat at `PRIORITY_LOW`, so `TaskSerialWrite` at `HIGH` always preempted
it regardless of what either task was doing.

`-D configUSE_TIME_SLICING=1` in `platformio.ini` closes this without touching
`src/mavlink.cpp`. At `configTICK_RATE_HZ = 1000` (the framework default, also
unchanged), the scheduler round-robins same-priority ready tasks every 1 ms tick
whether or not either voluntarily yields. The alternative considered was an explicit
`taskYIELD()` after each inbound message in `TaskMavlink`'s loop — cheaper in blast
radius, since it touches only the one task, but narrower: it would leave
`TaskSdWrite`/`TaskCli` at `PRIORITY_LOWEST` exposed to the same structural issue
(`TaskSdWrite` blocks on SPI during a card write with no known internal yield point,
and shares its priority with `TaskCli`), and it depends on every future task body at
a shared priority remembering to yield, rather than on a scheduler guarantee that
does not forget. Rejected for being a narrower fix to a problem that is not specific
to `TaskMavlink`.

Cost: none measured. This is scheduler policy — which same-priority ready task runs
next — not a new structure, so it adds no RAM or flash. It also does not change any
task's own stack high-water mark: preemption timing changes when a task runs, not
how deep its own call stack goes at any point it is interrupted, so the board
readings already taken for this change (`tasks.md` §5) stand. `pio run`/`pio check`/
the HIL suite were re-run with the flag on regardless, since a global scheduler
policy change is worth confirming rather than assuming inert.

### The table drives absolute deadlines, and `SYSTEM_TIME` keeps its 500 ms offset

Entries are `{function, interval_ms, last_ms}`. A pass emits **everything due**, then
advances each fired entry by `last_ms += interval_ms` rather than to "now", so a late
pass does not push the cadence forward — this is what replaces `vTaskDelayUntil`'s
drift-free property.

Everything due, not one message per pass: the USB side of `add-usb-dual-protocol`
rations because it writes into a port with a finite transmit buffer and wants to fail
one attempt rather than spin. This path posts to a queue that reports fullness
through `xQueueSend`, so there is nothing to ration.

`SYSTEM_TIME` is seeded half an interval behind `HEARTBEAT`. Today the two alternate
on a 500 ms `vTaskDelayUntil`, so a ground station sees them 500 ms apart; firing both
in one pass would put two frames back to back and change the wire timing. The claim
that this change is invisible from the ground only holds if the interleave is kept.

### The wait is the time to the next deadline

`xQueueReceive`'s timeout becomes `next_due - now`, floored at zero. `portMAX_DELAY`
goes away for this task, which makes `ARCHITECTURE.md` §3's "Consumer tasks block on
`xQueueReceive` with `portMAX_DELAY` and cost nothing when idle" false as written and
is why that line is in the file list.

The task is not idle-free any more: it wakes at least once a second regardless of
traffic. That is what the two deleted tasks were already doing between them, at a
higher combined rate, so the scheduler sees strictly fewer wakeups than before.

### Withholding `BATTERY_STATUS` becomes a table flag

The reduced configuration expresses itself today by `setup()` not calling
`xTaskCreateStatic` for `TaskMavlinkBatteryStatus`. Since the storage is declared
unconditionally, that withholds the behaviour without reclaiming the memory.
Afterwards the entry is simply absent from the schedule, and the memory is gone in
both configurations.

`TaskMavlink` starts in the reduced configuration, which is the property the recovery
mechanisms need — `src/mavlink.cpp:196` records that `TaskHeartbeat` carried them for
exactly that reason.

### Resource ownership is unchanged

Stated explicitly because it is the rule most easily broken by a change that moves
code between tasks. No file gains a resource and no resource gains a second file:

| Resource | Owner | Change |
|---|---|---|
| `LINK_SERIAL` (the UART) | `src/serial.cpp` | none |
| The MAVLink protocol, and every send to `serialWriteQueue` | `src/mavlink.cpp` | none — the sends already all live here |
| The ADC | `lib/Battery` | none; `battery.millivolts()` is called from a different task context, through the same wrapper, still from one place |
| Both clocks | `lib/SystemTime` | none |
| The SD card | `src/sdwrite.cpp` | none |
| Task and queue creation | `src/main.cpp` | two fewer tasks |
| The backup registers | `src/recovery.cpp` / `src/main.cpp` | none; the reduced-mode retry reaches them exactly as `TaskHeartbeat` did |

### `serialWriteQueue`'s backing is re-derived, not resized

Its producer count falls from three tasks to one, so §4's rule — depth, plus one block
per producer that can hold an unsent item, plus one per consumer — needs 6 blocks
where 8 are reserved, and the heap's commitment falls from 5808 to 5200 of 6136
usable.

Nothing is resized. Returning those 608 bytes would mean lowering
`configTOTAL_HEAP_SIZE`, and the margin they represent is worth more than the bytes,
which would come back as `.bss` only to be spent by the next change. The figure is
recorded here so the next change to a depth starts from 5200 rather than re-deriving
it from a stale 5808.

### One channel buffer, and why it rides along

`MAVLINK_COMM_NUM_BUFFERS` defaults to 4 and the library reserves per channel whether
or not the channel is used: 1164 bytes of `m_mavlink_buffer` and 96 per instantiation
of `m_mavlink_status`, all read from the current image's symbol table. Exactly one
channel is parsed, `MAVLINK_COMM_0` in `src/serial.cpp`.

This has nothing to do with folding tasks, and bundling unrelated work is usually
wrong. It is included because it is the same defect in a different place — storage
reserved for something that does not exist — because it is one line in `build_flags`
with no behavioural surface, and because leaving it in the backlog while this change
rewrites the RAM accounting means writing that accounting twice.

The cost of bundling it is a flag that two active changes now claim.
`add-usb-dual-protocol` introduces `MAVLINK_COMM_1`, which cannot exist while the
count is 1, so that change must raise it to 2 and absorb the 315 bytes. That is
recorded in `proposal.md` — Impact, in task 7.3, and as a comment beside the flag
itself, because `openspec/config.yaml` records a build-flag collision between two
changes as something that has already happened here without detection.

### `TaskMavlink` keeps 256 words

**The merged task needs the deepest path, not the sum of them.** The schedule
emission and the inbound dispatch run sequentially in one loop and each unwinds before
the next begins, so the requirement is `max(dispatch, heartbeat, systemTime, battery)`
plus the loop's own frame — not their total. This is the argument the whole approach
rests on, and it is worth stating plainly because "three task bodies in one task"
invites the opposite assumption.

The figures that exist are not reassuring enough to skip measuring. `TODO.md`'s
*`TaskLogger`'s stack margin is razor-thin* records the first live `ps` reading:
`TaskLogger` at 5 free of 96, and `TaskHeartbeat` at **28 free of 128** — so the
heartbeat path already uses about 100 words, with everything else having more room
than that. `TaskMavlink`'s own deepest path is the inbound dispatch, which puts a
`mavlink_command_long_t` and a `mavlink_timesync_t` on the stack and runs
`sendStatusText`'s string handling in the `default` case; it is very likely deeper
than 100 words and therefore the one that sets the requirement, which is why 256 is
expected to hold.

"Expected" is why task 5.1 measures it on the board against the baseline task 1.1
captures, and why 256 is not trimmed here to look like a further saving. If the union
does not fit, the array and the word count grow together and the change still returns
the two control blocks and most of the two stacks.

One knock-on in the other direction: `include/Data.h`'s `Tasks` loses two
`UBaseType_t`, so `Data` shrinks by 8 bytes and one `TaskLogger` cycle needs slightly
less stack. That entry asks to be re-checked after any change to `Data`'s size, so
this change owes it a reading rather than an assumption.

## Risks / Trade-offs

**The inbound path can now delay a scheduled emission.** → The dispatch is bounded
work with one exception: `systemTime.setUnixTime()` writes the DS1307 over I2C. It
short-circuits when the value is unchanged (`ARCHITECTURE.md` §5.3), so a ground
station repeating the time costs nothing, but a genuine set costs a bus transaction
inside the task that owes a heartbeat. The `vTaskDelay(pdMS_TO_TICKS(50))` at
`src/mavlink.cpp:315` looks worse and is not: `NVIC_SystemReset()` follows it two
lines later, so the board is rebooting either way. Measured on the board with a
ground station driving `SYSTEM_TIME` and `TIMESYNC`.

**One high-water mark now covers four code paths.** → `mavlinkAvailableStack` becomes
the union of the dispatch and all three producers, and a regression in one of them can
no longer be attributed from the log alone. Accepted: the union is the number that
decides whether the stack fits, which is what the log exists for. The task list
compares it against the worst of the three pre-change marks.

**The recovery tick becomes indirect.** → Today `vTaskDelayUntil` fires
unconditionally, so the stability-window clear and the 30-minute retry cannot be
missed. Afterwards they run on a schedule pass, which is guaranteed only because the
`HEARTBEAT` entry is present in every configuration and never conditional. That
invariant is now load-bearing and is stated in the code, not just here.

**Two record shapes can coexist on one card.** → `lib/SdData` appends into a ring that
may already hold seven-field records. Nothing in the firmware reads a record back, so
flight is unaffected; whoever reads the `.mpk` files has to tolerate both. Not
mitigated — the alternative is keeping two fields that describe tasks that do not
exist.

**`add-usb-dual-protocol` needs rewording afterwards.** → Its `design.md:106-107`,
task 2.2 and task 2.3 all describe a two-producer world. The work those tasks
represent is unaffected; only their wording is. Listed in `proposal.md` — Impact so
that whoever picks that change up finds it stated rather than discovering it.

## Migration Plan

There is no migration in the deployment sense — the firmware is flashed whole. What
matters is order: this lands first, because `add-usb-dual-protocol` built on top of
today's 1468 bytes of headroom fails `ram_budget.py`.

Rollback is `git revert`. The only state that outlives a reflash is the SD card, and
the ring tolerates mixed record shapes in both directions.
