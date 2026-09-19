## Context

See `proposal.md` — Why. Two facts from `ARCHITECTURE.md` §4 shape this design:

- `serialReadQueue` and `serialWriteQueue` both declare `sizeof(mavlink_message_t*)`
  as their element size — a 4-byte pointer — but the queue's *depth* only bounds
  what can be enqueued at once. What actually backs a queued item is a
  `pvPortMalloc`'d block on the FreeRTOS heap, sized so that depth *plus* one
  block per producer that can hold an unsent item *plus* one per consumer
  holding an unreleased one never runs out. That backing, not the queue
  structure itself, is `configTOTAL_HEAP_SIZE`'s only job today.
- `src/mavlink.cpp` owns the protocol — "nothing else in the firmware knows what
  a `msgid` is" — and this design keeps that true. Only `src/mavlink.cpp` may
  call a `mavlink_msg_*_pack()` or `mavlink_msg_*_decode()` function; `src/serial.cpp`
  stays a transport that moves opaque values between a port and a queue.

Read fresh today (`pio run -e uno_r4_minima`, clean build): committed 29092 B of
32768 (88.8%), headroom 3676 B. `ucHeap` (`configTOTAL_HEAP_SIZE`, `0x1800`) is a
fixed 6144 B `.bss` symbol; `sizeof(mavlink_message_t)` is 291 B and
`sizeof(mavlink_status_t)` is 24 B, both confirmed against the image's symbol
table in earlier changes (`fold-periodic-telemetry-into-mavlink-task`).

## Goals / Non-Goals

**Goals:**

- Remove every `pvPortMalloc`/`vPortFree` around MAVLink traffic, and with it the
  failure mode where a missed free leaks the 8 KB heap fatally within minutes.
- Shrink `configTOTAL_HEAP_SIZE` to what the one remaining heap consumer
  (`sdWriteQueue`) needs, reclaiming the difference as `.bss` headroom.
- Preserve `src/mavlink.cpp`'s exclusive ownership of the protocol — `src/serial.cpp`
  must not gain a `mavlink_msg_*_pack`/`_decode` call of its own.
- Leave on-wire behavior byte-identical: same messages, same rates, same identity,
  same drop-on-full semantics.

**Non-Goals:**

- Changing either queue's depth. The depths (8 and 5) were derived from how many
  sends can be due in one schedule pass, which does not depend on how an item is
  stored — that reasoning is untouched.
- Decoding inbound messages early to shrink `serialReadQueue`'s item size. This
  would need `src/serial.cpp` to know which fields matter per `msgid`, which is
  exactly the ownership boundary this design keeps intact. `serialReadQueue`'s
  item stays a full `mavlink_message_t`, just held by value instead of by
  pointer — see Decisions below for what that costs and why it is still worth
  doing.
- Building the USB secondary link (`TODO.md` backlog). This change only clears
  the RAM headroom that link needs; it adds no new port, task or queue.

## Decisions

### The outbound queue item becomes a tagged union, `LinkMsg`

`include/LinkMsg.h` (new, data-only, no protocol logic — safe for `src/serial.cpp`
to include without gaining protocol knowledge):

```c
enum class LinkMsgKind : uint8_t {
    Heartbeat,
    SystemTime,
    BatteryStatus,
    TimesyncReply,
    StatusText,
    CommandAck,
    NamedValueInt,
};

struct LinkMsg {
    LinkMsgKind kind;
    union {
        struct { uint64_t unix_usec; uint32_t boot_ms; }       system_time;
        struct { uint16_t millivolts; int8_t remaining;
                 uint8_t charge_state; }                       battery;
        struct { int64_t ts1; uint8_t tsys, tcomp; }           timesync;
        struct { uint8_t severity; char text[50]; }            statustext;
        struct { uint16_t command; uint8_t result; }           command_ack;
        struct { char name[10]; int32_t value; }                named_value_int;
        // Heartbeat carries no payload: `kind` alone is the whole message.
    };
};
```

`STATUSTEXT` stays in the union rather than getting a second, shallower queue.
It sets the size at 64 B (`kind` + `severity` + `text[50]`, rounded for
alignment); without it the common case would be 16 B. The alternative — a
second queue or a small text pool just for `STATUSTEXT` — was rejected: at
depth 5 the difference is (64-16) x 5 = 240 B, two orders of magnitude below
what this change reclaims, and a second queue structure is exactly the kind of
apparatus `CLAUDE.md` asks not to add without the RAM pressure to justify it.

Every `send*` function in `src/mavlink.cpp` (currently `pvPortMalloc` + a
`mavlink_msg_*_pack` call + `xQueueSend`) instead fills a `LinkMsg` value and
`xQueueSend`s it directly. `TaskSerialWrite` (`src/serial.cpp`) receives the
value and calls a new `src/mavlink.cpp` function to turn it into wire bytes:

```c
// include/MavlinkPack.h (new declaration, defined in src/mavlink.cpp — the
// only file that may call a mavlink_msg_*_pack function; named MavlinkPack.h
// rather than Mavlink.h because this filesystem resolves #include <MAVLink.h>
// case-insensitively, and a same-named header here shadows the library's own)
void mavlinkPack(const LinkMsg &intent, mavlink_message_t *out);
```

`TaskSerialWrite` calls `mavlinkPack(intent, &msg)` into a local
`mavlink_message_t` on its own stack, then `mavlink_msg_to_send_buffer` as
today. This is the same shape `src/serial.cpp:29` already uses on the read
side — a local, not a heap or static buffer — so it costs stack the task
already has room for (192 words, unchanged). `mavlinkPack` is also exactly the
seam a future USB writer would call to emit the same messages without
duplicating the pack logic, though building that is out of scope here.

*Alternative rejected:* have `TaskSerialWrite` call `mavlink_msg_*_pack`
directly, switching on `LinkMsgKind` itself. Cheaper by one function call, but
it means `src/serial.cpp` names MAVLink pack functions and field layouts,
which is precisely the protocol knowledge `ARCHITECTURE.md` reserves to
`src/mavlink.cpp`. Not worth the boundary for one indirection.

### The inbound queue item stays a full `mavlink_message_t`, but by value

`serialReadQueue` changes from `sizeof(mavlink_message_t*)` to
`sizeof(mavlink_message_t)` as its element size. `TaskSerialRead` already holds
a fully parsed message in a file-static local; it now `xQueueSend`s that
struct by value instead of `pvPortMalloc`-ing a copy and sending the pointer.
`TaskMavlink`'s dispatch is unchanged except that it reads a received value
instead of dereferencing-then-freeing a pointer — the `switch (msg.msgid)` and
every decode call stay exactly as they are, because they still see a complete
message.

This does not shrink the read side's footprint the way the write side's does —
291 B is 291 B whether it sits in a heap block or a queue's static array — but
it still removes `pvPortMalloc` from the read path entirely, and it trades a
*worst-case* heap reservation (backing blocks for depth + producer + consumer
margin, 3040 B) for an *exact* static one (depth x 291 B = 2328 B), which is
smaller on its own besides being off the heap.

### `configTOTAL_HEAP_SIZE` shrinks to cover only `sdWriteQueue`

Once neither MAVLink queue touches the heap, the only remaining consumer is
`sdWriteQueue` (depth 4, backing 6 blocks x 56 B = 336 B today). The new size
must be measured against a real build rather than picked round — heap_4 carries
per-block overhead this document should not guess — but a provisional `0x200`
(512 B) gives ~176 B of margin over the current 336 B figure, comparable to
the margin the heap carries today. Task 4 below replaces this provisional value
with a measured one before this change is considered done.

### Recomputed RAM impact — confirmed against a clean build (task 4.1)

| | Today | After |
|---|---|---|
| `ucHeap` (`configTOTAL_HEAP_SIZE`) | 6144 B, fixed | 512 B, fixed (`0x200`) |
| `serialReadQueueStorage` | 32 B (8 x pointer) | 2328 B (8 x 291 B) |
| `serialWriteQueueStorage` | 20 B (5 x pointer) | 320 B (5 x 64 B) |
| `sdWriteQueueStorage` | 16 B (unchanged) | 16 B (unchanged) |
| **Fixed cost, these three queues + heap** | **6212 B** | **3176 B** |

That table's arithmetic was confirmed, not just projected: a clean
`pio run -e uno_r4_minima` before this change read 29092 B committed, 3676 B
headroom; the same build after every source change in this file, with
`configTOTAL_HEAP_SIZE=0x200`, read **26056 B committed, 6712 B headroom** —
exactly the ~3036 B reclaimed this table predicts. `sizeof(LinkMsg)` measured
64 B exactly (the `.bss` delta backs this out precisely), matching the
`static_assert` bound in `include/LinkMsg.h`.

**That 6712 B did not survive contact with the board.** `TaskSerialWrite`
overflowed its stack within seconds of boot — the slow 2 s-on/2 s-off LED
pattern, confirmed against the actual hardware — because the risk this
document already flagged (a new 291 B `mavlink_message_t` local on a 192-word
stack) was real, not just theoretical. Grown to 384 words. `TaskMavlink`
measured 41 of 256 words free before the overflow was even reached — thinner
than the ~127 `add-mavlink-housekeeping-telemetry` recorded, and before
`COMMAND_LONG`'s branches were exercised at all — grown to 384 words as well,
proactively, rather than waiting for a second crash. Final state, confirmed
by a clean build: **27336 B committed, 5432 B headroom.** Both stacks now
measure comfortably (146/384 and 169/384 words free respectively) under the
`pio test -e bench` HIL run, which exercises the branches a boot-time smoke
test does not.

**Dynamic confirmation (task 4.2) could not be completed.** The board fell
into, and stayed in, the reduced configuration during this session — first
from the stack-overflow crash's watchdog-reset loop, and confirmed to persist
past the 5-minute stability window and a forced reset, which means the
*cumulative* fault counter is over threshold, not just the consecutive one.
Neither `Logger` nor `SdWrite` runs in that configuration, so `sdWriteQueue`
is never touched and its heap margin cannot be exercised from here — this is
a pre-existing project limitation (`TODO.md` already documents having no
ground command to clear the cumulative counter), not something this change
introduces. The static fact stands regardless: `0x200` covers `sdWriteQueue`'s
336 B worst case with 176 B to spare. Whoever next has the board in the
normal configuration should confirm the minimum-ever-free heap stays
comfortably above zero under real logging load.

**Why this is enough for the USB link this unblocks.** Not this change's job to
build, but worth checking the sequencing decision it exists to serve: a future
symmetric USB queue pair, sized the same way as the table above (a `LinkMsg`
write queue and a full-`mavlink_message_t` read queue), would cost roughly
another 2328 + 320 = 2648 B. Against the **confirmed 5432 B** headroom this
change actually leaves — not the 6712 B first projected, before the two stack
fixes above — that still fits, with about 2784 B to spare before even
counting that future change's own new task stacks (TODO.md's backlog entry
flags this cost too, and this session's own experience with `TaskSerialWrite`
and `TaskMavlink` is a concrete reason not to under-provision those either).
The margin is real but tighter than first thought; that change must still
re-derive this number against a fresh build when it lands rather than quote
either figure here.

### The CI grep guard extends to `pvPortMalloc`, scoped to two files

`.github/workflows/main.yml`'s existing step greps all of `src/` for
`xTaskCreate`/`xQueueCreate` and fails the build if either appears, because
both must be the `...Static` form. `pvPortMalloc` cannot be added to that
same whole-`src/`-tree pattern, because `src/logger.cpp` and `src/sdwrite.cpp`
still legitimately use it for `sdWriteQueue`'s heap-pointer protocol, which
this change does not touch (Non-Goals) — an earlier draft of this design
assumed this change removed the *last* `pvPortMalloc` from `src/`, which
undercounted: it removes every one from `src/serial.cpp` and
`src/mavlink.cpp` (eight pairs), not from `src/` as a whole.

The new step scopes the grep to exactly those two files instead:

```
if grep -nE '\bpvPortMalloc\b' src/serial.cpp src/mavlink.cpp; then
  echo "::error::src/serial.cpp and src/mavlink.cpp carry MAVLink queue items by value and must not allocate"
  exit 1
fi
```

This still turns "no allocation on the MAVLink path" from a convention into
something CI enforces, without false-failing on `sdWriteQueue`'s unrelated,
still-legitimate use. `lib/` is unaffected either way and keeps whatever
allocation it already does (e.g. ArduinoJson's `malloc` in
`src/sdwrite.cpp`'s dependency, which is a separate, already-tracked concern —
*The flight log is a private MessagePack format no tool can read*, `TODO.md`).

## Risks / Trade-offs

- **A future outbound message kind can grow `LinkMsg` past what these figures
  assume.** → The union's size is a single `sizeof(LinkMsg)` fact; a
  `static_assert` in `include/LinkMsg.h` bounding it (e.g. to 64 B, today's
  ceiling) turns a silent regression into a build failure the next time
  someone adds a variant without checking.
- **Removing the allocation-failure path also removes a signal.** Today a full
  heap surfaces as a `NULL` from `pvPortMalloc`, silently dropping the message
  (nothing counts it — a gap *Emit `SYS_STATUS`*, `TODO.md`, already names).
  After this change, the only failure mode left is `xQueueSend` finding the
  queue full, which was already the primary one and is already handled
  identically (free-on-failure collapses to simply not producing anything to
  free). No new gap, but worth stating: this change does not add the counting
  that entry wants, it just removes one of the two paths that would need it.
- **`TaskSerialWrite`'s stack gained one local `mavlink_message_t` (291 B) it did
  not carry before, and it overflowed.** Its original 192-word (768 B) stack
  already held a `uint8_t buf[MAVLINK_MAX_PACKET_LEN]` (280 B) for the wire
  bytes; adding the pre-pack struct did not merely thin the margin, it
  exceeded it — confirmed on the board (task 7.2) via the slow 2 s-on/2 s-off
  stack-overflow LED pattern within seconds of boot, since the boot
  `STATUSTEXT` is the first message this task ever packs. → Fixed by growing
  the stack to 384 words; it now measures 146 of 384 words free.
- **`TaskMavlink` gained the same kind of local: `xQueueReceive`'s destination
  is now `mavlink_message_t msg`, a 291-byte local, where it used to be a
  4-byte pointer.** `TaskMavlink`'s stack was 256 words (1024 B) with the ~127
  of 256 words free `add-mavlink-housekeeping-telemetry` had recorded before
  this change. Measured on the board (task 7.2) at 41 of 256 words free —
  thinner than a comfortable margin, and before `COMMAND_LONG`'s branches were
  exercised at all. → Grown to 384 words proactively rather than waiting for
  a second overflow; now measures 169 of 384 words free under the full
  `pio test -e bench` HIL run.

## Migration Plan

No gradual rollout applies to firmware with one flight image: this lands as one
commit, verified by `pio run` (RAM), the unmodified `pio test` HIL suite (behavior),
and a board session (stack high-water marks, `ps`/`free` — the CLI still exists at
this point; its removal is a separate, later change). Rollback is `git revert`
and `pio run -t upload`, same as any other firmware change.
