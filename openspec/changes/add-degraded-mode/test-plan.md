## Test plan

7 requirements, 17 scenarios, 17 rows. Written by the qa engineer.

**This plan was written out of order.** `tasks.md` already existed with 42 steps when it
was made, so the plan could not drive the task list the way the schema intends. Its value
here is audit instead: every row names the task that already claims it, and the two lists
at the end record what the task list misses. A reader a year from now should not mistake
this for a plan the tasks were built from.

Methods: **T** test · **A** analysis · **I** inspection · **R** review of design ·
**D** demonstration, a person at the board performing a physical act.

| Row | Requirement → Scenario | Method | Where it runs | Receipt | Board |
|---|---|---|---|---|---|
| TP-1 | A task that stops yielding resets the board → A task blocks forever | D | flash a temporary build whose lowest-priority task spins without yielding; observe on a passive listener; reflash the flight build after | one continuous tlog in which `SYSTEM_TIME.time_boot_ms` stops advancing then restarts near zero within `WDT_TIMEOUT_MS`, and the first `HEARTBEAT` after it decodes `custom_mode`'s reason byte as watchdog `[4.4]` | yes, and hands |
| TP-2 | A task that stops yielding resets the board → The slowest legitimate cycle completes | D | flight build, card fitted, undisturbed for longer than the stability window and many `TaskSdWrite` cycles | `data*.mpk` records whose `uptime` climbs monotonically with no return to near-zero, and a tlog over the same window where `custom_mode`'s cumulative byte never changes `[4.3, 4.6]` | yes |
| TP-3 | A task that stops yielding resets the board → A fault handler is reached | D | flash a temporary build that provokes one fault hook — an undersized stack for a deliberate deep call, or an allocation that cannot succeed; reflash after | the hook's LED pattern observed, a reset within `WDT_TIMEOUT_MS`, and the next boot's `custom_mode` phase byte equal to that hook's marker with the boot `STATUSTEXT` naming it `[no task — see (a)1]` | yes, and hands |
| TP-4 | The cause of a reset is known at the next boot → The board resets after a hang | D | provoke a watchdog reset, then a clean external reset or power-on, listener attached across both boots | one tlog with two successive first-heartbeats: reason byte = watchdog on the first, = external or power-on on the second, not watchdog again `[2.2, 2.3]` | yes, and hands |
| TP-5 | The cause of a reset is known at the next boot → Initialisation stops partway | D | provoke a hang inside one named initialisation phase with the WDT already open — a temporary spin at the clock phase, or a card that stalls SPI | the next boot's `custom_mode` phase byte equal to that phase, and the boot `STATUSTEXT` naming that step `[3.3, but see (b)2 — its act stops producing a reset once 6.2 lands]` | yes, and hands |
| TP-6 | The cause of a reset is known at the next boot → The board is powered on | D | remove power entirely — USB unplugged, battery disconnected — wait, reapply | the first `HEARTBEAT` after power-up decoding `custom_mode`'s reason byte as power-on `[2.2]` | yes, and hands |
| TP-7 | Repeated failure selects a reduced configuration → Three consecutive boots fail | D | three watchdog or software resets, each inside the stability window. **Not** the RESET button, which the requirement forbids from advancing the count | the consecutive byte reading 1, then 2, then 3 on successive boots, and the fourth boot's heartbeat carrying `MAV_STATE_CRITICAL`, `base_mode` without `MAV_MODE_FLAG_AUTO_ENABLED`, and no `BATTERY_STATUS` in a 12 s sample `[5.4, but see (b)4]` | yes, and hands |
| TP-8 | Repeated failure selects a reduced configuration → A fault appears after every boot has been declared stable | D | reset repeatedly, each time only after the stability window has elapsed, until the cumulative threshold | the cumulative byte stepping to ten across the capture while the consecutive byte stays 0, and the heartbeat turning `MAV_STATE_CRITICAL` at the threshold boot `[5.5]` | yes, and hands |
| TP-9 | Repeated failure selects a reduced configuration → A boot runs normally | D | let the board run past the stability window, then reset it once | the consecutive byte observed as 0 after the window, and after the reset `check_telemetry.py:10:test_heartbeat_at_1hz:PASS` plus the four other telemetry and identity cases firing PASS in the same `run.py` output `[5.6]` | yes, and hands |
| TP-10 | The reduced configuration stays reachable and commandable → The board boots reduced with a ground station attached | D | with the board already reduced, attach the host and send `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` | `check_telemetry.py:10:test_heartbeat_at_1hz:PASS` from `run.py --filter heartbeat` in the reduced state, plus a `COMMAND_ACK` for that command with `MAV_RESULT_ACCEPTED` in the tlog `[7.3, 8.3]` | yes, and hands |
| TP-11 | The reduced configuration stays reachable and commandable → The card and the clock are both absent in the reduced configuration | D | pull the SD card, unplug the DS1307 from I2C, drive the board into the reduced configuration | `check_telemetry.py:10:test_heartbeat_at_1hz:PASS` for transmit and `check_timesync.py:11:test_timesync_is_answered:PASS` for receive-and-act, from one filtered run, both firing rather than `IGNORE` `[6.6]` | yes, and hands |
| TP-12 | Absent hardware degrades rather than halts → The real-time clock does not respond | D | unplug the DS1307, capture open before power-up, then set the time from the host | three things in one session: the boot `STATUSTEXT` naming the clock absent; `test_heartbeat_at_1hz`, `test_system_time_at_1hz` and `test_battery_status_every_2s` firing PASS; and `check_clock.py:21:test_inbound_system_time_sets_the_clock:PASS` `[6.4, but see (b)1 — the third receipt is unobtainable as `lib/SystemTime` stands]` | yes, and hands |
| TP-13 | Absent hardware degrades rather than halts → The card is missing | D | remove the SD card, open the capture, then power up | the boot `STATUSTEXT` naming the card absent, in a capture started before power-up, plus `test_heartbeat_at_1hz`, `test_system_time_at_1hz` and `test_battery_status_every_2s` firing PASS `[6.5]` | yes, and hands |
| TP-14 | The ground can see the state without asking for it → A ground station connects long after a degraded boot | D | leave the reduced board running; minutes later start a **passive** listener that transmits nothing — `run.py`, not MAVProxy | `check_recovery.py:test_first_heartbeat_reports_reduced_state:PASS` — a new case asserting, on the *first* `HEARTBEAT` it sees, `MAV_STATE_CRITICAL`, `base_mode` without `AUTO_ENABLED`, and `custom_mode` decoding to reason, phase and both counts — plus a capture containing no host-originated frame before it `[7.3, but see (b)5 — MAVProxy transmits on connect]` | yes, and hands |
| TP-15 | The ground can see the state without asking for it → Normal operation is not reported as degraded | T | a new passive case in `test/test_hil/check_recovery.py`, run by `pio test` alongside the existing suite | `check_recovery.py:test_heartbeat_reports_operational:PASS` — state `MAV_STATE_ACTIVE`, `AUTO_ENABLED` set, reduced bit clear — and `check_recovery.py:test_observed_message_set_is_closed:PASS` — the 12 s sample contains exactly `HEARTBEAT`, `SYSTEM_TIME`, `BATTERY_STATUS` — plus the five existing telemetry and identity cases firing PASS in the same run `[7.4, 9.6; neither asserts the set is closed today, see (a)2]` | yes |
| TP-16 | The reduced configuration can be left → The ground commands a return to normal | D | from a reduced board, send the reboot command from the host | `COMMAND_ACK` with `MAV_RESULT_ACCEPTED` in the tlog, then `SYSTEM_TIME.time_boot_ms` restarting near zero, the next heartbeat at `MAV_STATE_ACTIVE` with both counter bytes zero, and `BATTERY_STATUS` back at 0.5 Hz `[8.1, 8.3]` | yes, and hands |
| TP-17 | The reduced configuration can be left → The link is the failing part | D | put the board in the reduced configuration, keep the inducing fault present, attach only a passive listener, send nothing, wait out the full retry interval | one uninterrupted capture spanning more than two retry intervals in which `time_boot_ms` restarts once at the interval, the heartbeat turns `MAV_STATE_ACTIVE`, then returns to `MAV_STATE_CRITICAL` after the fault recurs, with no less than the retry interval between successive entries into the reduced state `[8.4]` | yes, and hands |

## Rows nothing can reach

No row is wholly unreachable, but three carry a clause no method here can close. All three
are still rewordable today, which is the point of saying so now.

- **TP-17, "rather than looping rapidly between the two."** A capture shows one retry and
  one return. "Does not loop rapidly" is a property over unbounded time. Reword to a
  bounded, observable statement — *the interval between successive entries into the
  reduced configuration is at least the retry interval* — which the TP-17 capture does
  evidence. Otherwise the receipt silently stops at half the scenario.
- **TP-2, "no reset occurs."** Same shape. What is demonstrable is "no reset in an
  observation of N hours". The THEN should name the window, so the receipt has a defined
  size instead of being satisfied by whatever run happened to be made.
- **TP-13, "a ground station sees the absence reported."** For a missing card the only
  report is the one-shot boot `STATUSTEXT`, so a station connecting afterwards can never
  see it — which is the failure R6's own rejected alternative describes. `custom_mode`'s
  four bytes are fully spent on reason, phase and the two counters, so there is no
  continuing indicator and no spare field for one. Either the scenario says *a ground
  station attached across the boot*, or the design finds a continuing indicator. TP-12
  escapes this because `SYSTEM_TIME` reporting an unset epoch is a continuing indicator;
  TP-13 has no equivalent.

## Notes

**Board and hands.** 17 of 17 rows need the board; 15 need hands. That is the shape of
this change, not a gap in the plan. `dfu-util` reports no DFU capable device, so every row
is *unverified pending the board*, and no row was softened to compensate. `TODO.md`'s
*Recover the bricked board* gates all 17.

**The SD log cannot witness this change.** `include/Data.h` has no field for the reset
reason, the phase marker or either counter, and no task adds one. In the reduced
configuration `TaskLogger` and `TaskSdWrite` do not start, and with the card absent they
do not either. Every receipt above must therefore be captured live over the link with a
host attached at the time; there is no read-the-log-afterwards fallback except TP-2's
`uptime` trace. Related: `Data::energy` is never assigned in `src/logger.cpp`, so battery
figures in that log are zero — no row may lean on them.

**Degraded-state HIL cases must self-skip, and that makes `IGNORE` ambiguous.** A case
asserting the reduced state would FAIL on a healthy board and turn `pio test` red. New
cases in `check_recovery.py` must raise `hil.NoLinkError` when the board is not in the
state they check, which `run.py` prints as `IGNORE` — indistinguishable from "no board".
Consequence for every D row: the receipt is the PASS line from a *filtered* run made after
the act, never a green whole-suite run.

**Flashing is itself a watchdog reset, and it moves the counters.** By design the DFU path
lets the WDT fire, so each `pio run -t upload` and each `pio test` produces a watchdog
reset that advances the cumulative count. At a threshold of ten, roughly ten flashes put
the board into the reduced configuration on their own, at which point
`test_battery_status_every_2s` fails for a legitimate reason and 9.6's receipt becomes
unobtainable. Every session needs the counters cleared first — 9.8 does this only "after
commissioning" — and the first boot after any flash reports *watchdog*, not power-on,
which confounds TP-4 and TP-6 unless the reset under test is separated from the flash.

**Order the acts.** TP-7 must precede TP-10, TP-11, TP-14, TP-16 and TP-17, which all need
a board already reduced. TP-4 needs TP-1's build still on the board. TP-16 clears the
counters, so it ends any reduced-state session. TP-8 costs ten stability windows and TP-17
at least two retry intervals: budget hours, with an uninterrupted capture for both.

**Wiring.** With the flight build the link is on D0/D1, so all of this needs a USB-TTL
adapter or the radio. `pio test -e bench` moves it to USB but is not the flight
configuration, and `pio run -t upload` must follow. Never use a 1200-baud touch to reset
this board — on the Minima it enters DFU and stays there.

**`pio test -e libs` is not proposed for any row.** It covers `lib/` only and is
destructive. It becomes relevant to exactly one thing: if the no-RTC time fallback lands
in `lib/SystemTime` (see (b)1), a Unity case with the DS1307 disconnected is the only
method that reaches it without hands at the board — and even then it erases `data*.mpk`
and `index.bin` and must be followed by `pio run -t upload`.

**A figure whose achievability is not mine to judge.** Task 4.6 raises `WDT_TIMEOUT_MS` to
what `TaskSdWrite` needs. Whether the RA4M1 WDT can express that value at all belongs to
the firmware and hardware engineers, and TP-2's receipt depends on the answer. Settle it
before 4.6 rather than inside it.

**Evidence integrity if a partial task set is ever started.** `src/logger.cpp` calls
`uxTaskGetStackHighWaterMark` on all seven handles. A handle left `NULL` by a conditional
`setup()` makes that call return the *calling* task's mark, silently corrupting the stack
evidence the whole project relies on. Task 5.3's configurations happen not to start
`TaskLogger` alongside an unstarted task, but that is an accident of the current split and
should be an explicit invariant.

## Regression scope

- **An existing spec scenario is contradicted with no delta behind it.**
  `openspec/specs/mavlink-link/spec.md`, *Board powered with nothing attached*, promises
  "all tasks reach their steady-state cadence" and "housekeeping records continue to be
  written to the SD card at 1 Hz". Both are false in the reduced configuration and false
  with no card, yet `proposal.md` states *Modified Capabilities: None*. Either
  `mavlink-link` gets a MODIFIED delta qualifying that scenario to the normal
  configuration, or an archived requirement carries a demonstrably false scenario. For the
  systems engineer to resolve; recorded here because it decides what an HIL run may claim.
- **Cases that must run again in the normal configuration after this lands:** all nine
  existing HIL cases — `check_telemetry.py` ×6, `check_clock.py`, `check_timesync.py`,
  `check_silence.py`. `check_clock.py` writes to the DS1307 and must be re-run after any
  RTC-absence experiment restores the hardware.
- **No test is deleted or weakened by this change as written**, so no `REMOVED` requirement
  is needed. That changes if a degraded-state case is later added to the default set
  without self-skipping.
- **Nothing in `tasks.md` updates `test/test_hil/README.md`**, and `tasks.md:118` and
  `:150` both hard-code "nine HIL cases". Adding `check_recovery.py` makes both wrong. A
  task should update the README and record the scenario each new case covers — see
  *Record which scenario each HIL case covers* in `TODO.md`.

## Audit of the existing task list

A consequence of writing this out of order, and the most useful thing in the document.

### (a) Scenarios no existing task exercises

1. **TP-3, a fault handler is reached.** Task 3.2 only inspects the source; 4.4 stalls an
   ordinary task, which is not the fault-hook path. Nothing on the board provokes
   `vApplicationStackOverflowHook` or `vApplicationMallocFailedHook` and observes the reset
   out of it. Add a `[board]` step: flash a build that overflows one stack on purpose,
   confirm the reset within the timeout, confirm the next boot's phase byte is that hook's
   marker, reflash the flight build.
2. **TP-15's closed-message-set clause.** Task 7.4 says the message set "match[es] the nine
   HIL cases", but no case asserts the observed set is exactly `HEARTBEAT`, `SYSTEM_TIME`,
   `BATTERY_STATUS` — only three rates, the identity, and the absence of `BAD_DATA`. A new
   outbound message would pass unnoticed. Add `test_observed_message_set_is_closed` and
   name it in 7.4.
3. **The no-RTC time fallback itself.** R5 requires a ground-set time to be accepted with no
   DS1307, and no task touches `lib/SystemTime`. See (b)1: this is both a missing
   implementation step and a missing receipt.
4. **`setup()` completing inside the watchdog timeout.** The refresh lives in the idle hook,
   which does not run until `vTaskStartScheduler()`, so all of `setup()` after the WDT is
   opened must fit in `WDT_TIMEOUT_MS`. With 4.1's "deliberately short value", a
   legitimately slow `SD.begin(9)` becomes a reset loop. No task measures `setup()`'s
   duration. Add a `[board]` step recording time-to-first-heartbeat with the card fitted,
   and derive a floor on `WDT_TIMEOUT_MS` from it.

### (b) Tasks whose claimed verification does not produce the receipt its scenario needs

Most serious first.

1. **6.1 and 6.4 — "verify the existing inbound `SYSTEM_TIME` path still sets the clock"
   cannot hold with no DS1307.** `lib/SystemTime/SystemTime.cpp:11` is
   `if (_ds1307.begin() && RTC.begin())` — short-circuit, so with the DS1307 absent the
   RA4M1's internal `RTC` is never begun. `getUnixTime()` then returns 0, and
   `setUnixTime()` writes to an unstarted `RTC` and calls `_ds1307.adjust()` on hardware
   that is not there. No task in this change touches `lib/`. TP-12's third receipt is
   unobtainable and the requirement's "SHALL accept a time set by the ground" is
   unimplemented. Add a task that begins the internal RTC regardless of the DS1307 and
   makes `setUnixTime()` skip the DS1307 write when absent; its receipt is
   `check_clock.py:21:test_inbound_system_time_sets_the_clock:PASS` with the DS1307
   unplugged.
2. **3.3 stops working once 6.2 lands.** "Remove the SD card, let the board reset, confirm
   the following boot reports the card phase" — after 6.2 a missing card is a degradation,
   so `setup()` does not stop and the board never resets at the card phase. The two tasks
   are mutually exclusive as written, and TP-5 is left with no act. Re-scope 3.3 to a phase
   hang that survives the new policy, and state that a *missing* card is no longer a
   phase-marker demonstration.
3. **3.2's inspection cannot pass against the current tree.** It asks to verify the hooks
   "still contain no `Serial`, no `delay()`", but `vApplicationStackOverflowHook` in
   `src/hooks.cpp:22-30` contains `taskDISABLE_INTERRUPTS()`, then `while (!Serial) {}`,
   then two `delay(2000)` calls — while the same file's line 35 carries the comment
   *"delay() cannot be used from a fault hook: it waits on a tick that has stopped"*. The
   file documents the rule its own hook breaks. Worse for the receipt: "write a phase
   marker before they blink" puts the write after `while (!Serial) {}`, which with
   interrupts disabled never completes, so a stack overflow resets with the marker
   unwritten and is indistinguishable from a generic hang — exactly what the marker exists
   to prevent. The marker write must be the first statement of each hook, ahead of
   `taskDISABLE_INTERRUPTS()`, and 3.2's inspection claim must be corrected or paired with
   the step that changes the hooks.
4. **5.4 names no act that can produce the receipt.** "Force three consecutive resets" — but
   the requirement says an external reset SHALL NOT advance the consecutive count, so the
   RESET button, the only hands-only reset, is excluded by construction, and a power cycle
   may clear `VBTBKR` entirely (9.7). Name the mechanism: three watchdog resets from a
   stall build, or three software resets from a commanded path.
5. **7.3 contradicts itself.** "Verify with MAVProxy … having issued no request" — MAVProxy
   transmits on connect, its own heartbeats and a parameter fetch, so a MAVProxy session can
   never evidence the "issued no request" clause. Use a passive listener; `run.py` only
   receives. Make the receipt the new `check_recovery.py` case's PASS line plus a capture
   containing no host-originated frame before the first heartbeat.
6. **4.1 fixes no ordering.** If the WDT is opened after the clock or card phase, a hang in
   an earlier phase produces no reset and TP-5 is undemonstrable for those phases. Require
   it opened before the first phase milestone.
7. **4.6 and 4.5 produce figures with nowhere to live.** "Derived from 4.3's observations
   rather than guessed" yields a number but names no artifact recording the measurement, so
   the receipt is whatever anyone remembers. Same for 4.5's upload-latency measurement. Both
   should require the measured value written into `design.md` or the commit message beside
   the flag.
8. **9.6's receipt degrades itself.** Every acquisition of it is a flash, and every flash
   advances the cumulative counter; around the tenth the board boots reduced and
   `test_battery_status_every_2s` fails legitimately. Require the counters cleared
   immediately before the run, and record the counter values from the first heartbeat
   alongside the PASS lines so the run's context is on the receipt. Also correct "nine" if
   `check_recovery.py` is added.
9. **9.7 asks a question with no home for the answer.** Whether `VBTBKR` survives loss of
   VCC decides whether TP-6's and TP-7's power-cycle acts preserve the counters, and the
   design says the retry interval "must not be trusted until it is known either way".
   Require the outcome recorded in `design.md`'s Risks section, and make TP-17's receipt
   reference it.
10. **8.1 adds an outbound message.** `src/mavlink.cpp:160-168` decodes `COMMAND_LONG` and
    replies to nothing — there is no `COMMAND_ACK` anywhere in the file — so an ack is new
    on the wire, while TP-15 asserts the message set is closed. Not a contradiction with
    `mavlink-link` as written, which fixes rates and identity rather than a closed set, but
    it must be a deliberate call rather than a side effect found later by a closed-set case.
    State in 8.1 that `COMMAND_ACK` joins the outbound set, and list it in the expected set
    for `test_observed_message_set_is_closed`.

## Authorship and what the qa engineer stands aside from

Written by the `qa-engineer` agent and transcribed by the author, who verified every
checkable claim against the tree before transcribing: `SystemTime.cpp:11`,
`src/hooks.cpp:22-35`, the seven `uxTaskGetStackHighWaterMark` calls, `include/Data.h`'s
fields, `hil.NoLinkError`, `mavlink.cpp:160-168`, the *Board powered with nothing attached*
scenario, the proposal's *Modified Capabilities: None*, and the existence of all 26 task
ids cited. All held.

The qa engineer does not review this document, having written it. Whether each method would
prove anything on this device belongs to the firmware and hardware engineers; whether every
scenario reached a row, and whether the three rewordings under *Rows nothing can reach* are
acceptable, belongs to the systems engineer. Whether `WDT_TIMEOUT_MS`, the stability
window, the thresholds of three and ten, and the retry interval are the right quantities is
nobody's question here — only that each has a receipt, and today the timeout and the
interval do not.
