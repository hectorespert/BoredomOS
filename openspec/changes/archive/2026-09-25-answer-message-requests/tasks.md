# Tasks

Steps marked **[BOARD]** need the assembled board on USB; **[HANDS]** ones also need someone at it
(reaching the reduced configuration, a second port). Without the board the only checks that run
are `pio run` on both environments (`uno_r4_minima`, `libs`), the CI grep that guards static
creation, and `pio check`; none of them says whether a task body works.

## 1. Baseline, then the message plumbing

- [x] 1.1 **[BOARD]** Before touching code, flash the current tree, arm housekeeping with
      `MAV_CMD_SET_MESSAGE_INTERVAL` (message id 252, 1 s) and record `TaskMavlink`'s and the
      write task's (`src/link.cpp`) stack high-water marks and the `RAM budget` line
      (`committed 29728 B`, `headroom 3040 B` on the tree this change was written against).
      Verify by writing the numbers into this line's note: 1.4 and 4.3 have nothing to compare
      against without them.

      **Baseline** (tree at `1b2ca91`, flashed with `pio run -t upload`, normal configuration:
      `HEARTBEAT.base_mode` 132 carries `AUTO_ENABLED`; read over USB after 14 s of the
      housekeeping stream at 1 s; words free, high-water marks):
      `Mavlink` 129 of 384, `UartWrite` 131, `UsbWrite` 130, `UartRead` 31, `UsbRead` 58,
      `Logger` 56, `SdWrite` 69. `HeapFree`/`HeapMin` read 0, the known trap. `RAM budget`:
      `committed 29728 B of 32768 (90.7%)`, `headroom 3040 B (minimum 1024)`. The task that
      calls `mavlinkPack` is the pair `UartWrite`/`UsbWrite`.
- [x] 1.2 In `include/LinkMsg.h` add `LinkMsgKind::MessageInterval` (`uint16_t id;
      int32_t interval_us`), `MissionCount` (`uint8_t target_system, target_component,
      mission_type`) and `ProtocolVersion` (no payload). Verify `pio run` builds with the
      `static_assert(sizeof(LinkMsg) <= 64)` still in place.
- [x] 1.3 In `src/mavlink.cpp`, add the three `mavlinkPack` cases with the `_pack_chan` form
      (`MISSION_COUNT` with `count = 0` and `opaque_id = 0`; `PROTOCOL_VERSION` 200/200/200 with
      a `static const` zero array for both hashes) and one sender each, of the existing
      `void(uint8_t port)` shape where a port is all they need. Verify `pio run` on both
      environments with no new warning.
- [x] 1.4 **[BOARD]** Flash and confirm the periodic traffic is unchanged: `pio test` (the HIL
      suite) passes with the same cases it passed at 1.1, and the high-water marks and `RAM
      budget` headroom are unchanged within the words the new code accounts for. Record both.

      **Result.** 1.1 recorded stack marks but did not run the HIL suite, so this run is the
      suite's baseline, not a comparison: with the plumbing only (three `LinkMsgKind` values,
      three senders, three `mavlinkPack` cases, none reachable yet) `pio test` reports 44 cases,
      35 passed, 9 skipped, 0 failed (skips: the reset-restart, RESET-button and reduced-
      configuration cases, and the five that need `HIL_UART_PORT`). `RAM budget` is unchanged:
      `committed 29728 B`, `headroom 3040 B`. Stack marks read after that full HIL run, so not
      comparable with 1.1's freshly-booted ones (a mark only ever falls): `Mavlink` 125,
      `UartWrite` 131, `UsbWrite` 130, `UartRead` 41, `UsbRead` 43, `Logger` 56, `SdWrite` 67.
      **4.3 compares against these, not against 1.1's.** `HEARTBEAT.custom_mode` read
      `0x03020603` afterwards: 2 consecutive boots against a threshold of 3, so counters are
      cleared with `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` before each further flash.

## 2. `REQUEST_MESSAGE`, `GET_MESSAGE_INTERVAL` and the mission list

- [x] 2.1 In `src/mavlink.cpp` add the `const` id → {sender, schedule row} table of
      `design.md` and name the schedule rows beside `kHousekeepingScheduleIndex`. Verify by
      reading it against the design's table: ids 0, 1, 2, 147, 148, 300 served, 252 present
      only for `GET`, every other id absent. `pio run` builds.
- [x] 2.2 Add the `MAV_CMD_REQUEST_MESSAGE` branch to the `COMMAND_LONG` chain: validate the
      float parameter as a number in 0–65535 before narrowing; ACK, then the sender; `DENIED`
      for an id outside the served set and for 252; `TEMPORARILY_REJECTED` for 147 when
      `reducedConfiguration`. Verify `pio run` on both environments.
- [x] 2.3 Add the `MAV_CMD_GET_MESSAGE_INTERVAL` branch: the same validation; ACK
      `ACCEPTED`, then `MessageInterval` with `enabled ? interval_ms * 1000 : -1` read from
      `schedule[port][row]`, `-1` for 148 and 300, `0` for an id not in the table. Verify
      `pio run` on both environments.
- [x] 2.4 Add `case MAVLINK_MSG_ID_MISSION_REQUEST_LIST:` to the outer switch: decode, enqueue
      `MissionCount` addressed to `msg.sysid` / `msg.compid`, mission type echoed. Verify by
      reading that a `MISSION_REQUEST_LIST` no longer reaches `default`. `pio run` builds.
- [x] 2.5 Run `pio check`. Verify it reports no finding on a line this change added; record the
      count against today's (2 LOW, in `lib/Battery/Battery.cpp` and `src/mavlink.cpp`).

      **Result.** First run reported 3: the new `findMessage` range-`for` drew `useStlAlgorithm`.
      Rewritten as an index loop; second run reports 2 LOW, the same two as before
      (`Battery.cpp:28` `shadowFunction`, `mavlink.cpp` `voltages_ext` `constVariable`, whose
      line moved with the insertions). This change adds no finding. Also recorded: `RAM budget`
      unchanged (`committed 29728 B`, `headroom 3040 B`) because `kMessages` is 84 B in flash
      (`nm`: type `t`), flash 89516 -> 90248 B; the CI greps for `xTaskCreate(`,
      `xQueueCreate(` and the allocator over `src/` find nothing.

## 3. HIL coverage

- [x] 3.1 **[BOARD]** Add `test/test_hil/check_message_requests.py`, helpers modelled on
      `check_capabilities.py`, with cases for: a served id answered with its message and
      `ACCEPTED`; all six served ids; 60 requests for `HEARTBEAT` leaving `SYSTEM_TIME` at
      1 Hz; an unserved id and 252 answered `DENIED` with no message of that id; id 65538
      answered `DENIED` with no `SYSTEM_TIME`/`MESSAGE_INTERVAL`. Verify `run.py --list`
      discovers them and they pass against the flashed board.
      **Result.** `check_message_requests.py` has 12 cases; `run.py --list` discovers them. The
      served-ids, cadence, unserved-ids and invalid-id cases pass against the flashed board. Two
      were wrong at first and were fixed in the module, not the firmware: the harness decodes with
      pymavlink's `ardupilotmega` dialect, which does not define id 300, so `_name_of` no longer
      indexes the message map blindly, and `PROTOCOL_VERSION` is decoded by hand from the raw v2
      frame (the board's frame is correct: 200/200/200, zero hashes, payload trailing zeros
      truncated).

- [x] 3.2 **[BOARD]** In the same module, cases for `GET_MESSAGE_INTERVAL`: id 0 → ACK
      *before* `MESSAGE_INTERVAL` with 1000000; 148 → -1; an unknown id → 0; 65538 → `DENIED`;
      housekeeping armed at 2 s → 2000000 then, once disabled → -1. The last case disarms in a
      `finally`, since arming survives until reset and would leak into every later case. Verify
      they pass.
      **Result.** The four `GET_MESSAGE_INTERVAL` cases pass (periodic rates with the ACK
      before `MESSAGE_INTERVAL`; 148/300 -> -1 and 244/24/65535 -> 0; invalid id -> `DENIED`;
      housekeeping armed at 2 s -> 2000000 then -1 once disabled, disarmed in a `finally`).

- [x] 3.3 **[BOARD]** In the same module, cases for `PROTOCOL_VERSION` (200/200/200, both
      hashes zero, and absent from a quiet listen of several seconds) and for
      `MISSION_REQUEST_LIST` (type `MISSION` and type `FENCE` each echoed, `count == 0`, no
      `STATUSTEXT` in the following 2 s). Read `check_capabilities.py` and, if no case already
      asserts that no mission capability bit is set, add that assertion there. Verify they pass.
      **Result.** Pass. `check_capabilities.py` already asserts `capabilities ==
      MAV_PROTOCOL_CAPABILITY_MAVLINK2` exactly, which excludes every mission bit, so no assertion
      was added there. "Not sent unasked" reads `link.sample()`, the 12 s listen taken before any
      case ran, and `check_recovery.py`'s closed-set case would also fail on an unsolicited
      `UNKNOWN_300`.

- [ ] 3.4 **[BOARD][HANDS]** Cases that need a state or an adapter, each self-skipping by
      default like their neighbours: `BATTERY_STATUS` requested and `GET_MESSAGE_INTERVAL` for
      147 in the reduced configuration (`TEMPORARILY_REJECTED`, no message, `-1`), which needs
      the board latched by hand as `CLAUDE.md` describes; and, in `check_dual_link.py`, a
      request with broadcast target address answered on the requesting port only, and the
      housekeeping interval reported per port, which need `HIL_UART_PORT`. Verify by running
      them with the state or adapter present, or record here that they were not.
      **Not run, so left unticked.** The cases are written and load (`run.py --list`): the reduced-
      configuration one in `check_message_requests.py`, and two in `check_dual_link.py`
      (broadcast target answered on the requesting port only; the housekeeping interval reported
      per port). All three self-skip here, as their neighbours do: no USB-TTL adapter was attached
      and the board was not latched into the reduced configuration. They have never executed, so
      the spec scenarios for the reduced configuration and for the per-port claims are
      unexercised, not proven.

- [x] 3.5 **[BOARD]** Run the whole HIL suite, not only the new module, to confirm the
      closed-set checks that count traffic by type (`check_recovery.py`, `check_telemetry.py`)
      still hold with three new message ids possible on the wire. Verify it passes as at 1.1.

## 4. Documentation and closing checks

      **Result.** Whole suite, 58 cases: 45 pass, 12 are skipped (the nine from 1.4 plus the three
      above), 1 failed, and that one passed when run alone straight after. The failure and two
      earlier whole-suite runs (telemetry and clock cases reporting "4 messages in 1022 s" for a
      12 s window) were the host clock jumping while the machine slept mid-run, which
      `test_inbound_system_time_sets_the_clock` (host time against board time) is sensitive to;
      I inferred that from the elapsed figures, I did not prove it, and the clean isolated passes
      are the evidence. `check_recovery.py`'s closed-set case and `check_telemetry.py` pass, so no
      unsolicited message type appeared.

- [x] 4.1 Update `ARCHITECTURE.md`'s `Mavlink` bullet in §6: `MISSION_REQUEST_LIST` is answered
      (no longer falling to `default`), `REQUEST_MESSAGE` and `GET_MESSAGE_INTERVAL` join the
      `COMMAND_LONG` sub-switch, and the `const` id table is described beside the schedule
      table. Verify by re-reading the bullet against the code as it now stands.
      **Done** in the `Mavlink` bullet of §6.

- [x] 4.2 Read the proposal and design for figures the build has since falsified (RAM,
      headroom, table contents) and correct them in the same commit. Verify by re-running the
      `RAM budget` step of `pio run` and comparing.

      **Result.** `RAM budget` is still `committed 29728 B of 32768`, `headroom 3040 B`, as the
      proposal states; `kMessages` is 84 B in flash, as the design says; the table's ids,
      rows and results match the design's. One falsehood found and fixed: spec and design said
      the message-id parameter must be "a number in 0-65535", but the code (and the passing HIL
      case) also refuses a fractional one, so both now say "whole number".
- [x] 4.3 **[BOARD]** Read the stack high-water marks of `TaskMavlink` and the write task
      again, with housekeeping armed and after the requests of section 3 have run, and compare
      with 1.1. Verify the difference is recorded here; a task that lost more margin than the
      new locals explain is investigated, not accepted.

      **Result** (words free, after the full section-3 traffic, against 1.4's post-HIL marks):
      `Mavlink` 125 -> 104 (of 384), `UartWrite` 131 -> 122, `UsbWrite` 130 -> 121; `UartRead`,
      `UsbRead`, `Logger`, `SdWrite` unmoved or noise. The `Mavlink` loss of 21 words is what the
      new `COMMAND_LONG` branches account for: each `send*` they call builds a 64 B `LinkMsg`
      local (16 words) one call deeper than before, plus its frame; I did not measure the
      split. The write tasks' 9 words are the larger pack path for `PROTOCOL_VERSION` (22 B
      payload). Margin left is 104 and 121 words; nothing here is a reason to grow a stack.
- [ ] 4.4 Say plainly in this file, before archiving, which of 3.1–3.5 and 4.3 were run and
      which were not. A scenario nothing exercised stays in the spec as contract, and the
      archive step turns every unticked box above into a `TODO.md` entry naming this change.

      **Status now.** Run on the board: 1.1, 1.4, 3.1, 3.2, 3.3, 3.5 and 4.3. Not run: the three
      cases of 3.4 (reduced configuration, and the two that need an adapter on D0/D1), so the
      scenarios "BATTERY_STATUS requested with no battery reading", "BATTERY_STATUS in the reduced
      configuration reports no stream", "Requested on one port does not answer on the other" and
      "The housekeeping stream reports its armed state per port" are unexercised. Not run either:
      `pio test -e libs`, which this change does not need (it touches nothing in `lib/`), and the
      RESET-button and restart cases, unchanged by it. Left unticked deliberately; archiving
      turns 3.4 and this line into a `TODO.md` entry.
