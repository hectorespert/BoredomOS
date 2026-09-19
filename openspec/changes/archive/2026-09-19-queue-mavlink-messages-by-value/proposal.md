## Why

`serialReadQueue` and `serialWriteQueue` carry pointers to a heap-allocated
`mavlink_message_t` (291 B) for every message in flight, so the 6 KB FreeRTOS heap
exists almost entirely to back messages that carry 9-41 useful bytes, and every
producer/consumer pair around it is a place a missed `vPortFree` leaks the heap
fatally within minutes. Queuing what a producer *means* instead of a serialized
frame — a small by-value struct — removes that failure mode outright, and once
nothing depends on the FreeRTOS heap for MAVLink traffic, `configTOTAL_HEAP_SIZE`
can shrink toward what `sdWriteQueue` alone needs, returning several kilobytes of
`.bss` headroom. That headroom is what a planned second, symmetric MAVLink link on
USB (backlog, `TODO.md`) needs: a queue pair of its own, which cannot be afforded
twice at today's per-message heap cost.

## What Changes

- Both `serialReadQueue` and `serialWriteQueue` change from `mavlink_message_t*`
  items to a small by-value struct. A producer fills a local value and posts it
  directly — no `pvPortMalloc`, no `NULL` check, no `vPortFree` on a failed
  `xQueueSend`, no consumer free.
- `src/serial.cpp`: `TaskSerialRead` already owns a fully parsed
  `mavlink_message_t`; it copies that into the queue by value instead of
  allocating a heap duplicate. `TaskSerialWrite` receives a value and packs it
  into a local/static `mavlink_message_t` before calling
  `mavlink_msg_to_send_buffer`, rather than dereferencing and then freeing a
  pointer.
- `src/mavlink.cpp`: every `send*` function that today does
  `pvPortMalloc` + fill + `xQueueSend` fills the by-value struct and sends it
  directly. `TaskMavlink`'s inbound dispatch reads the value the queue handed
  it and frees nothing at the end of the switch.
- `configTOTAL_HEAP_SIZE` shrinks from `0x1800` (6144 B) to whatever
  `sdWriteQueue` alone needs, with margin measured against a real build rather
  than picked round — the FreeRTOS heap no longer backs any MAVLink traffic once
  this lands.
- Removes every `pvPortMalloc`/`vPortFree` pair around MAVLink traffic from
  `src/serial.cpp` and `src/mavlink.cpp` — eight pairs, not the "last two"
  an earlier draft of this proposal estimated before the actual call sites in
  `src/mavlink.cpp` were counted. `src/logger.cpp` and `src/sdwrite.cpp` keep
  theirs: `sdWriteQueue` is untouched by this change (see Non-Goals in
  design.md). This makes it possible to extend
  `.github/workflows/main.yml`'s existing `xTaskCreate`/`xQueueCreate` grep
  guard to also forbid `pvPortMalloc` in those two files specifically — a
  mechanical guarantee instead of a convention every MAVLink producer has to
  remember correctly, scoped to where it now actually holds.
- Retires the `CLAUDE.md` invariant "queues carry heap pointers, never values"
  with the reasoning it held for (291-byte items making by-value storage
  prohibitively expensive) and the reason it no longer applies.
- **No observable behavior change.** Same messages, same rates, same identity
  triple, same drop-on-full semantics when a queue is saturated. This is an
  internal representation change, not a protocol or timing change.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

None. `mavlink-link`'s requirements (port, rates, identity, cadence guarantees,
housekeeping semantics) are about on-wire behavior, which this change does not
touch — only how a message is carried between two tasks on the same board. No
spec-level behavior changes, so this change sets `skip_specs: true`.

## Impact

**Files.** `src/serial.cpp`, `src/mavlink.cpp`, `src/main.cpp` (queue creation and
the storage arrays declared beside it), a new small header for the by-value
message type, `platformio.ini` (`configTOTAL_HEAP_SIZE`),
`.github/workflows/main.yml` (optional grep extension covering `pvPortMalloc`),
`ARCHITECTURE.md` §4 (the queue memory ownership protocol, which this change is
exactly the described retirement of), `CLAUDE.md` (the "queues carry heap
pointers" convention), `TODO.md` (this entry, "Queue the message intent by value
instead of a packed `mavlink_message_t`", is deleted by this change).

**RAM**, read from a fresh `pio run -e uno_r4_minima` today (clean build): committed
29092 B of 32768 (88.8%), headroom 3676 B. `ucHeap` — the FreeRTOS heap array
sized by `configTOTAL_HEAP_SIZE` — is a fixed 6144 B `.bss` symbol regardless of
how much of it is used at any moment. Of that, MAVLink traffic backs a worst case
of 5168 B (`serialReadQueue` 3040 B + `serialWriteQueue` 2128 B, per
`ARCHITECTURE.md` §4's current table) and `sdWriteQueue` backs the remaining 336 B.
Once MAVLink items are carried by value, the heap only has to cover
`sdWriteQueue`'s 336 B; shrinking `configTOTAL_HEAP_SIZE` to match, with margin,
reclaims on the order of 5 KB of headroom. The exact new size and the resulting
headroom are a design.md decision, confirmed against a real build rather than
computed on paper, per `CLAUDE.md`'s "read fresh" rule.

**New static cost.** Each queue's storage array grows from `depth * sizeof(pointer)`
(4 B/slot) to `depth * sizeof(the by-value struct)`. The struct's size is a
design.md decision — whether `STATUSTEXT` stays in the same union as the other
message kinds swings it between roughly 16 B and roughly 64 B per item — but even
the larger figure at today's depths (8 and 5) totals under 600 B, dwarfed by the
multi-kilobyte heap reclaimed above.

**Other active changes.** None: `add-usb-dual-protocol` (the only other change
that referenced this queue design) has been abandoned rather than landed, so
there is nothing else in flight for this change to invalidate or be invalidated
by.
