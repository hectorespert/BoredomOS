## Why

`add-console-cli` gives the USB console a CLI by giving each protocol its own port:
MAVLink on `Serial1`, text on USB, and a `bench` build that swaps them when the HIL
suite needs the link reachable without a USB-TTL adapter. That split is what keeps it
simple, and it is also its ceiling — you can have the CLI or a ground station on USB,
never both, and choosing costs a rebuild and a reflash.

On the bench that is the wrong trade. The board is sitting on a desk with one USB
cable attached, and the two things you want from it — "show me the stack headroom" and
"let me watch the telemetry" — are on opposite sides of a build flag. madflight solves
this by letting one port carry both and deciding per byte which it is, and the same
approach fits here without disturbing the radio link at all.

## What Changes

- **USB carries both protocols.** The port starts in CLI mode and switches to MAVLink
  the first time a complete, CRC-valid frame arrives. The switch is one-way until
  reboot, as in madflight: no timer, no escape sequence, no mode state to get wrong.
- **The two links are independent, not a mirror.** USB gets its own frame parser, its
  own sequence numbering and its own transmit schedule. `Serial1` keeps the existing
  queue-based pipeline untouched. Nothing is tee'd between them; each generates its own
  copy of the same message set.
- **USB emits the same messages as the radio link** — `HEARTBEAT` and `SYSTEM_TIME` at
  1 Hz, `BATTERY_STATUS` every 2 s — so a ground station on USB sees an ordinary
  vehicle with no configuration. Those are nominal rates: the USB stream is produced at
  a lower priority than the radio's and drops frames rather than delaying anything, so
  under load it is the one that slips. That asymmetry is deliberate — the bench port
  must never cost the radio a deadline.
- **Both ports run the same message logic.** The construction of every outbound
  message and the handling of every inbound one move behind `include/MavlinkShared.h`, with
  one definition in `src/mavlink.cpp` and two callers. USB does not get its own copy of
  the identity triple or its own inbound `switch`, so "the same message set on both
  ports" is structural rather than a promise to keep. This refactors the `send*`
  functions and `TaskMavlink` in a working file — see `design.md`.
- **The USB transmit path does not allocate and cannot block**, following madflight's
  `telem_send`: pack into a stack buffer, check the port has room, write or drop and
  retry on the next pass, one message per pass. This is what makes a second
  independent stream affordable.
- **`bench` is deleted.** It existed only to move the link onto USB so the HIL suite
  could reach it without an adapter. USB now carries MAVLink in every build, so the
  environment, its `LINK_SERIAL` override and the "revert the link to USB" escape hatch
  all stop having a reason to exist. The suite runs against the firmware that flies.
- `test/test_hil/check_silence.py` is **removed**: it asserts USB carries no MAVLink
  frames, which is the behaviour this change deliberately reverses.

**This alters the MAVLink surface.** No new message id and no changed rate, but the
firmware goes from one endpoint to two. Both advertise the same identity — system id
`1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`, `MAV_AUTOPILOT_GENERIC` — because
they are two links to one vehicle, not two vehicles. Sequence numbers are per-port and
independent, which is what the protocol expects of separate links.

**Depends on `add-console-cli`** and must land after it. That change creates the
console owner, the task and the command set this one extends; without it there is no
CLI mode to switch away from.

## Capabilities

### New Capabilities

None. This change extends two capabilities that already exist rather than introducing
a third — the CLI and the link are each gaining a behaviour on a port they already
know about.

### Modified Capabilities

- `mavlink-link`: today it requires that the firmware "SHALL NOT exchange MAVLink
  frames over the USB CDC port", and that a build may revert the link to USB for bench
  work. Both change: USB carries frames in every build, the revert-to-USB escape hatch
  is removed along with `bench`, and the identity and cadence requirements have to hold
  across two simultaneous endpoints rather than one selected at build time.
- `console-cli`: today it requires that "no build carries both the MAVLink link and the
  CLI on the same port", and that the console port is named by a definition a build can
  move. That is exactly what this change reverses. It gains the mode-detection
  behaviour, the one-way switch, and what the CLI owes a port it now shares.

## Impact

**RAM.** This entry originally framed the cost against the 8 KB FreeRTOS heap, on the
assumption that a second parser state and a transmit buffer would be the main cost.
Both assumptions were wrong once implemented and board-measured (task 4.6 has the full
story); the real cost is almost entirely static task-stack sizing, not heap, and the
actual numbers are substantially different from the original estimate:

| Item | Cost | Where |
|---|---|---|
| New task | none — the console task from `add-console-cli` gains the MAVLink mode | — |
| New queue | none — the USB transmit path writes directly, by design | — |
| Shared inbound/outbound `mavlink_message_t` (`src/cli.cpp`'s `mavlinkMsg`) | 291 B | `.bss` |
| Telemetry schedule (3 entries) | 36 B | `.bss` |
| `MAVLINK_COMM_NUM_BUFFERS` 4 -> 2 (`platformio.ini`) | ~0 net | `.bss` — shrinks `src/serial.cpp`'s existing per-channel arrays by the same amount `src/cli.cpp`'s new usage of them costs |
| Console task stack (`src/cli.cpp`'s `TaskCli`), 128 -> 160 words | **+128 B** | `.bss` |
| Radio task stack (`src/mavlink.cpp`'s `TaskMavlink`), 256 -> 248 words | **-32 B** | `.bss` — reclaimed, not spent; see below |
| | **~388 B net, against the 1468 B of headroom `add-console-cli` archived with — 1080 B left, 56 B above the 1024 B floor** | |

What actually happened, briefly (task 4.6 in `tasks.md` has the board-by-board account):
the console task's stack didn't grow to a round provisional number and get measured
down — it **overflowed for real** at 128 words (`Watchdog` reset, `StackOverflowFault`
phase, reproduced three times) once `mavlinkHandleInbound`'s reply and the transmit
buffer nested several hundred bytes deep in the same call chain. Closing that gap
inside an already-tight RAM budget took real savings, not just growth: the transmit
buffer is 80 B, not `MAVLINK_MAX_PACKET_LEN`'s 280 B (this firmware's largest message,
`STATUSTEXT`, needs nowhere near the protocol's absolute worst case); the separate
inbound and outbound `mavlink_message_t` locals became one shared static, safe because
`mavlinkHandleInbound`'s cases always finish reading `msg` before writing `reply` (see
its own comment); and `TaskMavlink`'s own stack, over-provisioned at 256 relative to
its real 188-word measured peak, gave back 32 B toward the console task's growth once
that peak was itself properly measured (a first attempt at shrinking it, based on an
under-exercised 109-word reading, also overflowed — see task 4.6). Final measured
margins are thinner than this project's usual 35-45%: 10.6% for the console task,
24% for the radio task, both board-verified stable under sustained, deliberately
adversarial exercise rather than assumed from either number.

Deleting `bench` returns nothing to the heap but removes a build configuration from
every future change's test matrix.

**Files.** New: `include/MavlinkShared.h`. Modified: `src/mavlink.cpp` (the `send*`
functions split into builders plus a queue push, and `TaskMavlink` becomes a thin loop
over the shared dispatch), `src/cli.cpp` (mode detection and the MAVLink mode),
`platformio.ini` (delete `[env:bench]`, keep `LINK_BAUD` for the radio),
`include/Link.h` (`LINK_SERIAL` stops being an override point), `include/Cli.h`,
`src/main.cpp` (console task stack size), `ARCHITECTURE.md`, `CLAUDE.md`,
`test/test_hil/` (`check_silence.py` removed, cases retargeted at USB).

**A consequence worth stating up front.** The mode switch is one-way, so on any given
boot the USB port answers the CLI or talks MAVLink, never both in sequence. The HIL
suite currently opens the link and starts framing immediately, which would leave the
CLI unreachable for the rest of that run. Test ordering becomes load-bearing — see
`design.md`. A reset between phases was the original fallback if ordering alone were
not enough; task 1.3 found the board's only reset-like mechanism on this port (a
1200-baud touch) parks it in DFU indefinitely rather than resuming the firmware, so
that fallback does not exist and ordering has to carry the whole requirement.
