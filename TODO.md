# TODO

Pending work on BoredomOS: features to implement and changes to the existing ones.
Every entry is defined here before any code is written for it.

## Entry format

```markdown
### Short feature title

**Status:** proposed | defined
**Scope:** affected files or modules (`src/*.cpp`, `lib/*`, ...)

What it should do and why. If it adds a task or a queue, state the priority
(`include/Priority.h`), the stack size in words and who frees the memory of the
pointers travelling through the queue.
```

## To implement

### Finish what make-sddata-begin-idempotent left open

**Status:** defined
**Scope:** hands at the board and at the SD card; `openspec/changes/archive/`

`openspec/changes/archive/2026-09-20-make-sddata-begin-idempotent/` shipped 16 of its 18
tasks. `begin()` reopens, `end()` exists, and the Unity suite covers both — 17/17 on the
board, with the two new cases watched failing against the pre-fix `begin()` first. Two
steps remain, for different reasons.

- **4.2 — read the preamble off a real card.** Pull the card and parse `data0.BIN` with
  `pymavlink`'s `DFReader`; the procedure is in `ARCHITECTURE.md` §5.2, and note
  `mavlogdump.py` is not in the bundled `pymavlink`. **Until this is done the `onOpen`
  coverage gap that change set out to close is only half closed.** The Unity case proves
  the callback *ran*; it cannot prove the preamble is in the file, because
  `cleanSdFiles()` deletes the files a reader would need and the suite has no DataFlash
  reader. This is the same evidence *[Finish what replace-messagepack-log-with-dataflash
  left open]*'s 9.2 needs, so one card read closes both.

  The card is in a good state for it: `pio test -e libs` erased it, so `data0.BIN` is a
  file written from scratch by the flight firmware, whose first bytes are the preamble
  from the open `begin()` performed — a cleaner artifact than an appended-to file.

- **4.3 — the high-water marks are not comparable yet, and that is the real gap.** That
  step asked to confirm them unchanged. They were not: `SdWrite` read 83 words free
  against a reference of 87. What the measurement actually showed is that the reference
  cannot support the question — `Mavlink`, a task that change could not touch, moved
  **15 words** between two readings. A single sample is not a baseline.

  So this is not "re-read `SdWrite`". It is: take several readings across reboots, for
  every task, and record a range rather than a number, then put that range where the
  current figures live. Until then no change can honestly claim a high-water mark is
  unchanged, which is a check `CLAUDE.md` asks for after every task-body edit. Needs the
  board and the link only — `NAMED_VALUE_INT` armed with `MAV_CMD_SET_MESSAGE_INTERVAL`
  on message id 252 — not the card, and not hands.

Two things worth knowing before spending board time:

- **`pio test -e libs --without-uploading --without-testing` links the Unity binary
  without flashing.** It prints "Building in test mode", and `nm` on
  `.pio/build/libs/firmware.elf` then shows `runUnityTests()` and each case. That clears
  the link-failure class a `lib/` change can introduce — pulling a FreeRTOS translation
  unit into an environment whose `test_build_src` excludes the hook it needs — without
  spending a flash. `pio run -e libs` does not catch it.
- **The recovery counters were at `consecutive` 2 of 3 and `cumulative` 8 of 10** when
  that change finished (`custom_mode` `0x08020603`). The five-minute stability window
  clears the first; only `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` clears either. Worth sending
  that command before the next session of reflashes rather than discovering the reduced
  configuration latched.

### Finish what replace-messagepack-log-with-dataflash left open

**Status:** defined
**Scope:** `test/test_hil/`, hands at the board and at the SD card

`openspec/changes/archive/2026-09-20-replace-messagepack-log-with-dataflash/` shipped
38 of its 45 tasks. The seven that remain are recorded here because the change
directory stops being read once archived, and because **six of them are the only
evidence that would close this capability's central claim**: that the flight log is
readable by a standard tool. The format was validated against `pymavlink`'s
`DFReader` — including recovering 5 of 6 records from a deliberately truncated file —
but **against bytes built in Python, not bytes the firmware emitted**. Nobody has yet
read a file the board wrote.

All six need the card physically pulled and read on another machine; nothing over the
link reads the card, which is what *[Download the flight log over the MAVLink log
protocol]* and *[Serve the SD card over MAVLink FTP]* would change. Numbers below are
that change's own `tasks.md`.

- **9.2 — parse a real `data0.BIN` with a general-purpose tool.** The whole point of
  the change, and the first scenario of `openspec/specs/flight-log/spec.md`.
  `ARCHITECTURE.md` §5.2 records the procedure. Note `mavlogdump.py` is **not** in the
  PlatformIO-bundled `pymavlink` (it ships without `tools/`), so use `DFReader`
  directly.
- **9.3 — check the log's stack figures against the housekeeping stream.** Cross-
  validates log and telemetry against each other. The reference reading taken during
  that change: `Logger` 56, `SdWrite` 87, `Mavlink` 122, `UartRead` 41, `UartWrite`
  146, `UsbRead` 52, `UsbWrite` 136 words free.
- **9.4 — check the battery figures against `BATTERY_STATUS`.** This is what actually
  closes the defect where every `.mpk` ever written carried `millivolts: 0`. The
  firmware reported 3919 mV / 59 % when it was left running, so the log's `PWR`
  records should agree and none should read zero.
- **9.5 — cut power mid-write and confirm only the torn tail is lost.** The resilience
  claim. No script observes it.
- **9.6 — read back a `TIME` record after a ground clock set.** A `TIME` record from
  origin `ground` was written before the board was left running, so the evidence is
  already on the card.
- **1.3 — copy a surviving `data*.mpk` off the card.** Optional now: `cleanSdFiles()`
  was retargeted at `data*.BIN`, so the old MessagePack logs survive every Unity run
  and nothing in the firmware touches them again. They will sit there until removed by
  hand.
- **9.10 — that the log's time reference does not wrap is not demonstrable here.**
  It needs an uninterrupted run past what a 32-bit millisecond count can represent,
  ~49.7 days. The requirement is in the spec because the behaviour matters, and after
  archiving nothing in `openspec/specs/flight-log/spec.md` distinguishes it from a
  proven one. This entry is that distinction.

**One thing is covered nowhere and is not in the list above**, because no task claimed
it: the `onOpen` callback firing on the **first** open inside `SdData::begin()`. The
Unity suite could not reach it — `setUp()` calls `begin()` before any test body runs and
`begin()` was not idempotent, so a second call proved nothing. Rotation's callback **is**
tested. `openspec/changes/make-sddata-begin-idempotent/` is picking this up: it makes
`begin()` reopen, which puts the path within reach, and its task 2.2 adds the case. Note
what that case does and does not close — it asserts the callback **fired**, not that the
preamble reached the card, because `cleanSdFiles()` deletes the files a reader would need.
Only that change's task 4.2, with the card pulled, closes the rest.

Two things worth knowing before spending board time on any of the above:

- **`pio run -e libs` does not catch everything `pio test -e libs` does.** Adding a
  `vTaskSuspendAll()` to `lib/SystemTime` pulled FreeRTOS's `tasks.c` into that
  environment's link for the first time, which then needed a hook `src/hooks.cpp`
  defines and `test_build_src = no` excludes. `pio run -e libs` stayed green
  throughout. Expect this the next time a `lib/` change touches a FreeRTOS primitive.
- **`lib/SdData` had never been analysed by `pio check`.** `SdData.h` included
  `ArduinoJson.h` and cppcheck was silently giving up on the translation unit, hiding
  three pre-existing findings until the include left. Worth asking which other
  translation units are being skipped for the same reason — nothing reports a file it
  declined to parse.

### Finish what replace-console-cli-with-usb-mavlink-link left open

**Status:** defined
**Scope:** `test/test_hil/check_dual_link.py`, a USB-TTL adapter on D0/D1, hands
at the board

`replace-console-cli-with-usb-mavlink-link` made USB a second MAVLink endpoint and
landed with 34 of 38 tasks done. The four that remain all need hardware the
implementing session did not have: a USB-TTL adapter (or the radio) on D0/D1 as
well as the USB cable, and for one of them a human. Numbers below are that
change's own `tasks.md`, in the archive.

- **9.2 — both ports carrying telemetry simultaneously has never been seen.**
  Everything verified on the board so far was over USB alone. That the UART half
  still works is inferred from the code and from CI, not observed.
- **9.3 — per-port sequence numbering is only inferred.** `_pack_chan` is what
  keeps each port's `current_tx_seq` its own, and getting it wrong is silent:
  both ground stations still receive every frame, they just each see gaps.
  There is real partial evidence — the USB stream's `HEARTBEAT` sequence
  advanced in steps of 2-3, which is one port's own traffic, where a shared
  counter would have stepped 5-6, and the UART writer was packing all the while
  into an unattached port. But that reads one stream and infers the other.
- **9.4 — per-port housekeeping arming.** Arming message id 252 on one port must
  not arm the other. Only testable with two ground stations attached.
- **9.5 — a host that opens USB and stops reading must not reset the board.**
  **This is the most important one**, and no script observes it.
  `_SerialUSB::write()` loops without yielding when its buffer is full, so above
  idle priority it would keep `vApplicationIdleHook()` from refreshing the
  watchdog. `TaskLinkWrite` guards it with `availableForWrite()` and yields a
  tick on the drop path. Neither defence has been exercised against a real
  stalled host. Open the port, read nothing, watch the UART with MAVProxy:
  telemetry must hold its cadence, the SD log must keep writing, and the board
  must not reset. `test/test_hil/README.md` records it as a manual step.

`test/test_hil/check_dual_link.py` already contains the four automatable cases
and self-skips without `HIL_UART_PORT` set, so picking this up is attaching the
adapter and running `pio test`, not writing tests. **They have never executed
once** — three of them were corrected after review without ever having run, so
expect to debug the cases themselves as well as the firmware.

One more thing that session surfaced, worth knowing before spending board time:
four reflashes took the cumulative reset counter from 3 to 6 of the 10 that latch
the reduced configuration. It was cleared back to 1 with
`MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` over USB — which that change is what made
possible without an adapter — but the hazard is real and a long session will hit
it again.

### Finish what fold-periodic-telemetry-into-mavlink-task left open

**Status:** defined
**Scope:** `test/test_hil/`, hands at the board

`openspec/changes/archive/2026-09-18-fold-periodic-telemetry-into-mavlink-task/` folded
`TaskHeartbeat` and `TaskMavlinkBatteryStatus` into `TaskMavlink`'s schedule and
landed with 32 of 36 tasks done. The remaining four all needed hands physically at
the board, which this session did not have for the last stretch — each is already
explained in that change's own `tasks.md`, not silently skipped:

- **1.2 — no pre-change `mavproxy.py` trace was captured.** The code was already
  edited by the time this session got board access (see 1.1's sequencing note in
  the archived `tasks.md`), so there was nothing left to capture a "before" trace
  against. 5.4 substituted a different comparison basis instead — this project's
  own recorded pre-change CI passes of the same behavioural requirements, plus a
  fresh direct capture — which is why 1.2 is not simply redone as a formality if
  picked up: check whether 5.4's substitution is judged sufficient before spending
  the board time.
- **5.8 — pull the card, boot, confirm the firmware still reaches steady state.**
  Needs the SD card physically removed.
- **5.9 — read a fresh `data*.mpk` back and confirm it carries five per-task
  fields, and that a card still holding old seven-field records is written to
  without error.** **Overtaken — not to be done.**
  `replace-messagepack-log-with-dataflash` landed on 2026-09-20 and stopped `.mpk`
  being written at all. Reading a log the board wrote back off the card is carried by
  that change's own 9.2 instead, now in *[Finish what
  replace-messagepack-log-with-dataflash left open]*. The per-task field count was
  already wrong here as well — it is seven, not five, since
  `replace-console-cli-with-usb-mavlink-link`.
- **5.11 — the 30-minute reduced-configuration retry has still never been
  observed firing**, moved in this change but not watched end to end. Needs an
  uninterrupted capture spanning at least two retry intervals (over an hour),
  which is the same debt `add-degraded-mode` already left at its own task 8.4 —
  one board session could close both.

One more thing this change's own board time surfaced, worth carrying forward:
repeated `pio run -t upload` cycles during a single session can push the board
into the reduced configuration by themselves — it happened during this
change's own verification, at `cumulative=10`. Whoever next spends a long
session reflashing should expect this, not be surprised by it.

**Correction (found board-verifying `queue-mavlink-messages-by-value`,
2026-09-19): the claim above that no ground command clears the cumulative
counter was wrong.** `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` (`add-degraded-mode`)
already does it — its handler in `src/mavlink.cpp` calls
`Recovery::reinitialise()`, which zeroes both the consecutive and cumulative
counters, before resetting. Verified live: sent the command over a `bench`
build's USB MAVLink link to a board stuck in the reduced configuration
(cumulative over threshold from a stack-overflow crash's watchdog-reset loop),
got `COMMAND_ACK`/`MAV_RESULT_ACCEPTED`, and the next boot came up in the
normal configuration — confirmed by the heartbeat's `system_status` reading
`MAV_STATE_ACTIVE` instead of `MAV_STATE_CRITICAL`, and by housekeeping
listing all 8 tasks including `Logger` and `SdWrite`. The real limitation is
narrower than originally written: reaching this command needs an active
MAVLink connection, which the flight build only offers on `Serial1` — a board
stuck in the reduced configuration during a bench session needs the `bench`
build reflashed (or a UART adapter on D0/D1) to send it, rather than there
being no command at all.

### Finish what add-degraded-mode left open

**Status:** defined
**Scope:** `src/hooks.cpp`, `lib/SystemTime`, `platformio.ini`, `test/test_hil/`

`openspec/changes/archive/2026-09-13-add-degraded-mode/` shipped 35 of its 44 tasks,
board-verified against the recovered board. Seven remain, each already scoped in that
change's own `tasks.md` (numbers below refer to it). **6.1 and 6.4 are no longer here**:
6.1's short-circuit fix belongs to
`openspec/changes/archive/2026-09-19-improve-clock-synchronisation/`, which cannot read
the RTC's sub-second counter without it, and 6.4 moved there as a task. It is still
unclosed for the same reason it always was — the DS1307 cannot be disconnected on this
assembly, which is now known to be a standing property rather than one session's bad
luck, and which blocks 6.6 below as well.

- **3.2 / 3.3 — the fault hooks are still unsafe (review finding 9).**
  `vApplicationStackOverflowHook` in `src/hooks.cpp` writes the new phase marker
  correctly, but still does `taskDISABLE_INTERRUPTS()` then `while (!Serial) {}` then
  two `delay(2000)` calls — `delay()` cannot work with interrupts disabled, and the
  wait never completes with no host attached, which is the normal case in flight. Fix:
  marker first (already true), then `NVIC_SystemReset()` immediately, no blink. Once
  fixed, 3.3 needs rescoping too: it assumed pulling the SD card would produce a reset
  at the card phase, but task 6.2 (shipped) makes a missing card a degradation instead,
  so that act no longer reaches this hook at all.
- **4.6 — `WDT_TIMEOUT_MS` is still the placeholder value (1398 ms), not a measured one.**
  Needs `TaskSdWrite`'s worst case measured on the board, including a forced ring
  rollover (`lib/SdData/SdData.cpp`'s rotation can delete a file up to 1 GiB inside one
  call), then `platformio.ini`'s `-D WDT_TIMEOUT_MS` raised to match, bounded by the
  5.592 s hardware ceiling `design.md` in the archived change derives.
- **6.6 — the reduced configuration was never tested with both the SD card and the
  DS1307 absent together.** Blocked on hardware access, not code: the DS1307 was not
  disconnectable during that session. Needs hands at the board with both removed while
  three consecutive faults (or ten cumulative resets) put it in the reduced
  configuration.
- **8.4 — the automatic 30-minute retry out of the reduced configuration has never
  been observed firing.** Needs an uninterrupted capture spanning at least two retry
  intervals (an hour-plus), with the inducing fault held present for the second half to
  confirm it returns to reduced rather than oscillating.
- **9.5 — one new `pio check` finding against the pre-change baseline.** `13` LOW
  findings now, not `12`: `src/mavlink.cpp`'s new `sendCommandAck()` uses the same
  C-style pointer cast six other functions in that file already use. Decide once:
  convert all seven to `static_cast`, accept the one new hit, or suppress the rule —
  don't let it drift as an unexplained baseline change.
- **9.6 — the flight build (not `bench`) has never been run through `pio test` proper**,
  and the reduced-configuration counter values were never decoded from a live heartbeat
  and recorded alongside a HIL pass. Both need the board, with a USB-TTL adapter or the
  radio on D0/D1 since this one specifically needs the flight configuration, not bench.

Also worth doing before any of the above, found while reading back the archived
`review.md` and `test-plan.md`: **`test-plan.md`'s own header claims "17 scenarios, 17
rows" against the `fault-recovery` capability, but the delta actually has 21 — four
scenarios (the supply voltage sags, the RESET pin is pressed, the board leaves the
reduced configuration by its own action, the fault that caused it recurs) were never
given a row.** Some of their substance got informal exercise during board testing, but
none of it is tracked. If this capability changes again, add the missing rows first.

### Finish what add-mavlink-housekeeping-telemetry left open

**Status:** defined
**Scope:** `test/test_hil/check_housekeeping.py`, hands at the board

`openspec/changes/archive/2026-09-18-add-mavlink-housekeeping-telemetry/` shipped 10
of its 21 tasks CI-verified; a later board session (2026-09-18, `pio test -e bench`
against the physically attached board) closed out all but two more. What that run
found, for whoever next touches this:

- **Confirmed on real hardware:** 1.2 (`NULL`-handle skip), 2.2, 3.2-3.5 (arm, deny
  below the floor, disable, default-rate from clean boot and against an already-armed
  stream), 5.1, 5.4 (reduced configuration — the board happened to already be
  reduced, so this ran for real rather than self-skipping), and 6.2. All of
  `check_housekeeping.py` passed except the manual-reset case (see below).
  `TaskMavlink`'s stack high-water mark measured **127 of 256 words free** with
  housekeeping armed — comfortable margin. `ps` and the housekeeping stream agreed
  exactly on that figure, cross-validating both. The flight build was reflashed and
  reachable afterward (`pio run -t upload`, confirmed via `ps`/`free` on the
  console) — the board was not left on the bench build.
- **Still open — 5.3, the manual reset case.** `test_no_housekeeping_survives_a_reset`
  self-skips unless `HIL_MANUAL_RESET=1` is set, since it needs a human physically at
  the **RESET button** when prompted (never a 1200-baud touch —
  `test/test_hil/README.md:86-87`). Not attempted this session.
  ‑ **Still open — the normal-configuration (8-value) round-robin has never been
  observed.** The board was in the reduced configuration for this entire session
  (accumulated resets from repeated flashing across prior sessions — the same known
  hazard `fold-periodic-telemetry-into-mavlink-task`'s own TODO.md entry already
  describes), so `test_arming_covers_the_full_cycle_without_disturbing_existing_telemetry`
  and the reduced-configuration case both exercised the same 6-value set
  (`HeapFree`, `HeapMin`, `SerialRead`, `SerialWrit`, `Mavlink`, `Cli`); `Logger` and
  `SdWrite` have never actually appeared on the wire. Needs a session where the board
  is confirmed in the normal configuration first (a fresh SD card boot with no recent
  fault history, or the 30-minute automatic retry completing) before arming.

Also worth knowing if this change is touched again: its own `proposal.md`/`design.md`
originally estimated the RAM cost at ~337 B and were corrected, after implementation,
to a measured 8 B — the task-name table is `const` (flash, not RAM) and the queue
depth increase drew on already-reserved FreeRTOS heap slack rather than growing
`.bss`. See the archived proposal's Impact section for the full explanation before
assuming a similar table/queue change elsewhere costs what an estimate says it does —
but note the mechanism behind the second half is gone: there is no heap to find slack
in any more, so a deeper queue now costs depth times item size in `.bss`, every byte
of it.

### Finish what size-the-log-ring-and-batch-its-flushes left open

**Status:** defined
**Scope:** hands at the board and at the SD card

`openspec/changes/archive/2026-09-22-size-the-log-ring-and-batch-its-flushes/` shipped 18
of its 25 tasks. Everything code-side landed and is verified: `pio run`, `pio run -e libs`,
`pio test -e libs` (20/20), and `pio test` HIL (25/8/0) all pass with the new 4×1 MiB ring
and the batched flush. The seven that remain all need the board running unattended for a
long stretch, or a card pulled, or power physically cut — none of which the implementing
session had time for. Numbers below are that change's own `tasks.md`.

- **1.2 — how long one rotation took at the old 1 GiB default was never measured.** The
  task allowed leaving it unticked with a note if no practical file size made it
  measurable, and that is what happened: nothing was tried before the defaults changed
  under 2.1, so the number is simply gone now. Not worth chasing on the current, already-
  shrunk ring — the question was specific to the size being replaced.
- **5.1 — the ring has never actually rotated on a board doing its job.** Needs at least
  35 hours of continuous uptime so all four files are reused at least once; ~8.6 h only
  reaches the first rotation. Everything below depends on this running first.
- **5.2 — during and after 5.1, confirm rotation costs the board nothing.** The reset
  reason in the heartbeat's `custom_mode` unchanged, `time_boot_ms` monotonic across every
  rotation, fault counters no higher than they started. This is the check for the new
  requirement this change added, *Maintaining the log never resets the board* — a watchdog
  reset during a rotation would look like a mystery without it.
- **5.3 — pull the card and confirm the ring for real.** Four files of about 1 MiB each,
  `index.bin` present and naming one of them, each file parseable on its own with
  `DFReader` (`ARCHITECTURE.md` §5.2). Also the first evidence `index.bin` is written
  correctly by a rotation that actually happened, rather than by the Unity suite's
  1024-byte files.
- **5.4 — cut power mid-write and confirm the loss bound.** The proposal's BREAKING change
  is the spec's guarantee moving from "only the incomplete tail is lost" to "no more than
  one flush interval, about 4 KiB". **No script observes this, and until it is done the
  modified requirement has no supporting evidence at all** — the same shape of gap
  *[Finish what replace-messagepack-log-with-dataflash left open]*'s 9.5 left for the
  requirement this one renamed.
- **5.5 — repeat 5.4 on a board that has only just started logging.** Confirms the missing
  stretch is of the same order as 5.4's, which is the spec's *The amount at risk does not
  grow with uptime* scenario — the property that makes this a bounded policy rather than
  one that only looks fine on a long run.
- **5.6 — compare high-water marks against a baseline.** Blocked on more than board time:
  *[Finish what make-sddata-begin-idempotent left open]*'s 4.3 already found the existing
  reference figures are single samples, not a range, and therefore cannot support a claim
  of "unchanged". If that entry's readings are not done first, this one has nothing solid
  to compare against.

### Add the GY-87 IMU

**Status:** proposed
**Scope:** `src/sensors.cpp` (new), `src/main.cpp`, `include/SdRecord.h`,
`src/logger.cpp`, `src/sdwrite.cpp`, `src/mavlink.cpp`, `platformio.ini`

Add the **GY-87** module as the satellite's inertial unit, to know its attitude and
motion and report both over MAVLink as well as logging them to the SD card.

The GY-87 is a 10-degrees-of-freedom I2C module integrating three different chips:

- **MPU-6050** — 3-axis accelerometer and gyroscope, plus die temperature.
- **HMC5883L** — 3-axis magnetometer.
- **BMP180** — barometric pressure and temperature.

It goes on the same I2C bus the DS1307 already uses, so it adds no new pins to the
project's fixed wiring. **The module has already been ordered**, so the hardware is
not an unknown: what is left is checking it on arrival and deciding the software.

Two hardware problems to resolve **before writing any code**:

- **I2C address collision with the DS1307.** The DS1307 sits at `0x68`, a fixed
  address that cannot be changed, and the MPU-6050 answers by default at that same
  address. It is solved by pulling the module's `AD0` pin high to move it to `0x69`,
  but how that pin comes wired on the particular GY-87 board has to be checked: on
  many of them it is tied to ground and the module has to be modified. Without this
  the two devices collide and neither works.
- **The magnetometer hangs off the MPU-6050 auxiliary bus.** On the GY-87 the
  HMC5883L is not directly on the main bus: reaching it requires enabling the
  MPU-6050 *bypass* mode first. A bus scan that does not find it does not mean the
  module is broken.

To decide before implementing:

- **What is measured and at what rate.** Raw accelerometer, gyroscope and
  magnetometer data, or a fused attitude (roll/pitch/yaw) as well. Fusion means
  floating point arithmetic on every sample: it has to be measured against the tight
  stacks and the 1 Hz cadence of the rest of the firmware before committing.
- **Which libraries.** Each chip has its own and not all of them are lightweight.
  Since `use-static-allocation` the binding constraint is `.bss` headroom rather than
  the FreeRTOS heap, and `scripts/ram_budget.py` reports it after every link and fails
  the build when it runs out -- so the cost of a library is measurable before
  committing to it, and a proposal should state it.
- **Calibration.** The magnetometer needs offsets, and the gyroscope a zero. Where
  they are stored — compiled-in constants, a file on the SD card, parameters from
  the ground via *[Answer the GCS messages that are ignored today]* — is a design
  decision, not a detail.

Implementation points:

- `src/main.cpp:44` already declares `[[noreturn]] extern void TaskSensors(...)`
  with no implementation and no `xTaskCreate`: that is the reserved slot.
  `src/sensors.cpp` has to be created, with a priority from `include/Priority.h`
  and a stack size in words.
- Wrap the module in `lib/` in the style of `lib/Battery`, so that the task logic
  does not talk to three chips at once.
- Outbound MAVLink messages: `SCALED_IMU` or `RAW_IMU` for the inertial and
  magnetic data, `SCALED_PRESSURE` for the barometer, and `ATTITUDE` if fusion is
  implemented. Same identity triple as the rest: system `1`,
  `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.
- Extend the log's record set and `src/sdwrite.cpp`. These are quite a few new fields:
  review how much the log grows per second and what that does to the rotation rate of
  the `lib/SdData` ring. This is **additive**: since the log became DataFlash it takes
  a new record type with its own `FMT` definition rather than a widened existing
  record, so old logs stay readable and nothing has to be decided about compatibility.
- With three chips on the bus, I2C access is no longer exclusive to the RTC. How it
  is serialised against `lib/SystemTime` has to be decided, the same way the SD card
  is against `TaskSdWrite`.
- Checks in `test/test_libs/test_main.cpp`, which runs against real hardware.


### Add a temperature sensor

**Status:** proposed
**Scope:** `src/sensors.cpp` (new), `src/main.cpp`, `include/SdRecord.h`,
`src/logger.cpp`, `src/sdwrite.cpp`, `src/mavlink.cpp`, `platformio.ini`

Measure the on-board temperature and expose it through the two paths that already
exist: the housekeeping log on the SD card and the MAVLink telemetry to the ground.
It is critical data for a CubeSat: the LiPo and the SD card have a narrow operating
range and today there is no way to know at what temperature the board is flying.

To decide before implementing:

- **Which sensor.** If the IMU of *[Add the GY-87 IMU]* lands, the module already
  brings two temperature sources — the BMP180 and the MPU-6050 die — and this entry
  reduces to exposing that value, with no extra hardware or dependencies. Otherwise
  the alternative is a dedicated I2C part. Better decided **after** the GY-87.

  **The RA4M1 internal sensor is ruled out**, for three reasons found researching it:
  it reads the MCU die, not the environment, which is the wrong quantity for a LiPo/SD
  operating-range check; the Arduino core exposes no API for it, so reading it means
  raw register access to `ADC140`/`TSN_TSCDRH`/`TSN_TSCDRL` with no library, in this
  project or upstream; and it shares the `ADC140` peripheral that `lib/Battery` owns
  exclusively on `A0` (`CLAUDE.md`'s "only the owning file touches its resource"),
  which the other two options do not — they answer on their own I2C address instead of
  contending for a peripheral this project already assigned a single owner.
- **How many measurement points.** A single sensor, or several (battery, exterior)
  changes the shape of the data in `Data`.
- **Rate and caching.** `lib/Battery` caches for 125 ms; temperature changes far
  more slowly, so sampling can be much slower than the 1 Hz of `src/logger.cpp`.

Implementation points:

- `src/main.cpp:44` already declares `[[noreturn]] extern void TaskSensors(...)`
  with no implementation and no `xTaskCreate`: that is the reserved slot for this.
  `src/sensors.cpp` has to be created and given a priority from
  `include/Priority.h` and a stack size in words.
- Add the field to the log's record set, fill it in `src/logger.cpp` and dump it in
  `src/sdwrite.cpp`. Since the log became DataFlash this is a new record type with its
  own `FMT` definition rather than a widened existing one, so old logs stay readable
  and there is nothing to decide about compatibility.
- If the reading is not taken by `TaskLogger` itself, the value has to reach it
  without breaking the queue protocol: **a new `SdRecordKind` and payload in
  `include/SdRecord.h`, sent by value**. There is nothing to free and nothing to
  allocate. Note there is **no array to resize**: `sdWriteQueueStorage` in
  `src/main.cpp` is declared `4 * sizeof(SdRecord)`, so it follows the record
  automatically. What a larger payload does need is the `static_assert` in
  `include/SdRecord.h` updated — it pins the exact size, so it fails the build
  deliberately — and the queue's `.bss` cost rechecked against the headroom
  `scripts/ram_budget.py` reports, because four items grow with it.
- Outbound MAVLink message: pick a standard one (`SCALED_PRESSURE.temperature` in
  centidegrees, or `HYGROMETER_SENSOR`) and emit it with the same identity triple as
  the rest: system `1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.
- Add the check to `test/test_libs/test_main.cpp`, which only runs on real hardware. If
  the sensor is initialised with `configASSERT` in `setup()`, its absence will hang the
  board just as the RTC and the SD card do today.
- Watch the high-water marks after adding the task: the RAM margin is thin.

### Detect when the battery is charging

**Status:** proposed
**Scope:** `lib/Battery`, `src/mavlink.cpp`, `include/SdRecord.h`, `src/logger.cpp`,
`src/sdwrite.cpp`

Right now the firmware only knows *what voltage* the battery has, not whether
current is coming in from the panels. `src/mavlink.cpp` reflects that gap: it sends
a fixed `MAV_BATTERY_CHARGE_STATE_UNDEFINED` and `current_battery = -1`. From the
ground there is no way to tell a battery at 60 % climbing in sunlight from one at
60 % falling in eclipse, which is exactly the difference that matters when planning
power usage.

To decide before implementing:

- **Where the signal comes from.** `lib/Battery` only has `SolarCharger` on `A0`,
  and that library only exposes `readVoltage()`. There are two routes:
  - *Hardware:* read the charger's `STAT`/`CHG` pin on a GPIO. Reliable and
    immediate, but it adds wiring, and today the pinout is fixed in the code
    (SD `CS` on 9, battery on `A0`, DS1307 on I2C) — it would have to be documented
    there.
  - *Software:* infer it from the voltage trend. It touches no hardware, but it
    forces keeping history and fixing a threshold and a time window so that ADC
    noise is not mistaken for real charging.
- **Which states are distinguished.** Charging / not charging may be enough, or
  *charged* (end of charge) and *discharging* may also be of interest. This fixes
  which `MAV_BATTERY_CHARGE_STATE` values are emitted.

Implementation points:

- The reading lives in `lib/Battery`, next to `voltage()` and `remaining()`, with
  the same caching policy it already uses (125 ms) if the source requires it.
- Replace the fixed `MAV_BATTERY_CHARGE_STATE_UNDEFINED` of `sendBatteryStatus()`
  with the real state. Same identity triple as the rest: system `1`,
  `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.
- Add the state to the log's battery record and dump it in `src/sdwrite.cpp`, so that
  charge cycles can later be reconstructed from the log files. The battery figures now
  live in their own `PWR` record emitted by `TaskMavlink` at the rate the battery is
  actually read, so this extends that record rather than the 1 Hz one, and old logs
  stay readable.
- If the software route is chosen, the history cannot grow: fixed-size buffer, no
  `malloc` per sample.
- Check in `test/test_libs/test_main.cpp`, which runs on real hardware and can
  therefore validate the state with the board plugged in (charging) and unplugged.

### Download the flight log over the MAVLink log protocol

**Status:** proposed
**Scope:** `src/mavlink.cpp`, `lib/SdData`, `src/sdwrite.cpp`, `src/main.cpp`

Today the housekeeping log can only be recovered by pulling the card out of the
board: nothing exposes it over the link, which is not a realistic way of reading it
with the satellite assembled.

Read *[Serve the SD card as USB mass storage]* before starting this one. It moves the same
bytes far more cheaply, and if it works it may retire this entry and its FTP sibling — but
only for an operator holding the board. This path is the only one that works from orbit, so
the two are not substitutes; decide which problem is being solved.

This entry is the **log protocol**: `LOG_REQUEST_LIST` / `LOG_ENTRY` /
`LOG_REQUEST_DATA` / `LOG_DATA` / `LOG_REQUEST_END` / `LOG_ERASE`, ids 117–122. Its
sibling is *[Serve the SD card over MAVLink FTP]*, and **the firmware wants both**:
they are not alternatives and the two ground tools this project targets drive each of
them for different things.

| | log protocol | FTP |
|---|---|---|
| what it exposes | the flight logs, as an enumerated list | the filesystem, by path |
| the GCS finds them | by itself, from `LOG_ENTRY` | only if told the filename |
| QGroundControl | *Analyze → Log Download* | parameter and mission files, component metadata |
| MAVProxy | `module load log`, `log list` / `log download` | `module load ftp`, `ftp list` / `ftp get` |
| can write | no (`LOG_ERASE` only) | yes, which is the dangerous part |
| reaches `index.bin` | no | yes |

So this one is the path an operator uses to pull a flight log without knowing anything
about the card's layout, and FTP is the one that reaches everything else. All six
messages here are already in `common/` in the dialect this project compiles, so no
dialect change is needed.

Whichever lands first settles the card-access design for the other — see the
concurrency point below.

**Unblocked by `replace-messagepack-log-with-dataflash`, archived 2026-09-20.**
Downloading `.mpk` files would have accomplished little, since nothing on the ground
opens one. The log is DataFlash `.BIN` now and `pymavlink` reads it, which is what
makes this feature worth having.

Points to resolve before implementing:

- **Mapping the ring onto log ids.** `LOG_ENTRY` carries `id`, `num_logs`,
  `last_log_num`, `time_utc` and `size`. Decide how `data0..N.BIN` map onto ids given
  that the ring overwrites in place, and where `time_utc` comes from — the `TIME`
  record at the head of each file is the natural source.
- **Concurrency with `TaskSdWrite`.** `lib/SdData` keeps `_dataFile` open for writing
  while the logger dumps at 1 Hz, and the SD card hangs off SPI with `CS` on pin 9.
  Two tasks touching the card at once is corruption: either a mutex is needed, or
  access goes through `TaskSdWrite`, which already owns the medium. This is the main
  design decision of this feature, and it is **shared with *[Serve the SD card over
  MAVLink FTP]***: both need a reader alongside the writer, so solve it once, in
  whichever lands first, and let the other reuse it. Solving it twice, differently, is
  the way this ends up with two paths to the card and a corruption bug that only
  appears when both are in use.
- **RAM.** The usual constraint: `mavlink_message_t` alone is ~290 bytes and the
  stacks are tight, between 96 and 256 words. A download path with its read buffer
  does not fit without measuring; the log high-water marks have to be checked before
  and after.
- **Download time.** `LOG_DATA` moves 90 useful bytes in a 111-byte frame, ~4,6 KB/s
  at `LINK_BAUD`. Whether a whole file is viable is set by the ring default, which is
  where the size criterion lives — `openspec/changes/archive/2026-09-22-size-the-log-ring-and-batch-its-flushes/`
  derives it from download time and sets the default to 4 x 1 MiB, which is what makes a
  whole file viable here at all. Serving by offset is supported by the protocol (`LOG_REQUEST_DATA` takes
  `ofs` and `count`) and is worth implementing regardless.
- **Consistency of what is downloaded.** The active file is being written while it is
  read. Define whether it is served as is — the DataFlash `0xA3 0x95` header means a
  half-written record at the end is survivable rather than fatal, which FTP over
  MessagePack was not — or whether only the closed files of the ring are offered.
- **What QGroundControl calls the result.** QGC picks how to treat the downloaded
  bytes from the autopilot type, and this firmware announces `MAV_AUTOPILOT_GENERIC`.
  See the identity point in
  `openspec/changes/archive/2026-09-20-replace-messagepack-log-with-dataflash/`, which
  raised this and did not settle it.
- Keep the identity triple of the rest of the firmware: system `1`,
  `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.

### Serve the SD card over MAVLink FTP

**Status:** proposed
**Scope:** `src/mavlink.cpp`, `lib/SdData`, `src/sdwrite.cpp`, `src/main.cpp`

MAVLink FTP (`FILE_TRANSFER_PROTOCOL`) exposes the card as a filesystem: listing a
directory and reading a file by path, with the reference GCS (`ftp list` / `ftp get`
in MAVProxy, and QGroundControl's own uses). It is the sibling of *[Download the
flight log over the MAVLink log protocol]*, and the firmware wants both — that entry
has the table of which tool drives which, and why one does not replace the other. Both
should be read against *[Serve the SD card as USB mass storage]*, which does the same job
over the USB cable for a fraction of the work, and only for someone holding the board.

The short version: the log protocol is the one an operator reaches for to pull a
flight log, because the GCS enumerates them without being told anything. FTP is what
reaches **everything the log protocol cannot name** — `index.bin`, and any
configuration file the card grows later. *[Implement the MAVLink parameter
protocol]* raises storing parameters on the already-mounted card as one of its
options; if that is where it lands, FTP is how the ground inspects and repairs them,
and this entry stops being a convenience.

The slot is already marked: `src/mavlink.cpp` has an empty
`case MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL:` in the `TaskMavlink` switch.

Points to resolve before implementing:

- **Protocol scope.** MAVLink FTP is a state machine with sessions and opcodes
  (`ListDirectory`, `OpenFileRO`, `ReadFile`, `Terminate`, burst reads...). Decide
  the minimum subset: listing and reading read-only covers the use case; writing and
  deleting from the ground is another discussion, and a dangerous one over the file
  the firmware holds open. Note that read-only is also what makes this strictly
  additive to the log protocol rather than a second way to destroy the log —
  `LOG_ERASE` is already the sanctioned way to do that.
- **Concurrency with `TaskSdWrite`.** The same problem, with the same two answers — a
  mutex, or routing through the task that owns the medium — as the concurrency point
  of *[Download the flight log over the MAVLink log protocol]*. **Solve it once.** If
  that entry lands first this one inherits its answer; if this one lands first, build
  the access path so a second reader can use it.
- **RAM.** The usual constraint: `mavlink_message_t` alone is ~290 bytes and the
  stacks are tight, between 96 and 256 words. An FTP task with its session buffer
  does not fit without measuring; the log high-water marks have to be checked before
  and after. FTP's session state is the larger of the two features, so if both land,
  measure with both present.
- **Download time.** Each FTP packet moves ~239 useful bytes, so a whole ring file is
  only viable once the ring has a sane default, which `openspec/changes/archive/2026-09-22-size-the-log-ring-and-batch-its-flushes/`
  supplies: 4 x 1 MiB, derived from this very figure. Reads by offset are
  part of the protocol and let the ground fetch only the stretch of interest.
- **Consistency of what is served.** The active file is being written while it is
  read. Define whether it is served as is or whether only the closed files of the ring
  are offered. A half-written record at the tail is survivable rather than fatal now,
  because DataFlash records carry a `0xA3 0x95` resynchronisation header — but
  `index.bin` has no such property and a torn read of it is simply wrong.
- Keep the identity triple of the rest of the firmware: system `1`,
  `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.

### Serve the SD card as USB mass storage

**Status:** defined
**Scope:** `src/link.cpp`, `src/sdwrite.cpp`, `lib/SdData`, `src/logger.cpp`,
`src/mavlink.cpp`, `src/main.cpp`, `platformio.ini`, `ARCHITECTURE.md`

Both existing download paths are expensive: *[Download the flight log over the MAVLink log
protocol]* and *[Serve the SD card over MAVLink FTP]* each implement a protocol to move
bytes the host could read directly. madflight does neither — it exposes the card as a USB
mass storage device:

```c
usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);
// msc_read_cb -> sd.card()->readSectors(lba, buffer, bufsize/512)
```

Plug the board in, a disk appears, copy the file. No protocol, no rate limit, no partial
transfers to resume.

**The open question is whether it can coexist with the MAVLink CDC.** USB already carries
MAVLink as a CDC endpoint and `src/link.cpp` owns it, so this needs a composite CDC+MSC
device. The Renesas core does ship TinyUSB — it is named in *[The toolchain and uploader
are x86_64-only]* as the part GCC 14 rejects — but whether a composite descriptor is
reachable from this core, and what it costs in flash and RAM, is unknown and is the first
thing to find out.

**It breaks the ownership rule, and that is the blocking design question, not a detail.**
`ARCHITECTURE.md` §3 and §6 say `src/sdwrite.cpp` via `lib/SdData` is the only code that
touches the card, and that single rule is what makes the absence of mutexes safe. MSC
callbacks read and write raw sectors from the USB side — a second owner, on another task, at
another priority. So this entry cannot be implemented as "add MSC callbacks"; it has to say
who owns the card in each state and how the handoff happens, and that design does not exist
yet. Until it does, the entry is a sketch and not something to pick up.

**`SdData::end()` is not the handoff, and it is important not to mistake it for one.**
Closing the file does not stop logging. `TaskLogger` keeps posting `SdRecord`s once a second
and `TaskMavlink` keeps posting `PWR`, `TaskSdWrite` keeps receiving them, and
`SdData::write()` **silently returns without doing anything** when no file is open — so
enabling MSC on the strength of `end()` alone would race the producers and discard every
sample for as long as the host held the card. What is actually needed is a state transition:
stop the producers, drain `sdWriteQueue`, have `TaskSdWrite` stop consuming, *then* `end()`,
serve MSC, and reverse it on eject — with `begin()` reopening at the far side, which is why
`openspec/changes/make-sddata-begin-idempotent/` is a prerequisite rather than the answer.
That transition is the bulk of the work here and none of it exists.

Betaflight and madflight sidestep all of this by only enabling MSC when disarmed, which is a
state this firmware does not have.

If this works it may retire both protocol entries, which is a large saving — but it serves
only an operator holding the board, never a ground station on a radio link. The MAVLink
paths are the only ones that work from orbit, so this is a convenience for development, not
a replacement for them. Decide which problem is actually being solved before picking.

### Debug and release builds, with MAVLink tracing on the console

**Status:** proposed
**Scope:** `platformio.ini`, `src/mavlink.cpp`, `src/serial.cpp`, `src/cli.cpp`,
`CLAUDE.md`, `README.md`

Debugging the protocol today is done blind: there is no way to see which MAVLink
messages come in and go out without a GCS on the other end interpreting them. The
idea is to have two build profiles, with the debug one dumping the protocol trace
over the console port, in readable text.

Depends on `openspec/changes/archive/2026-09-13-add-console-cli`, not only on the
`move-mavlink-link-to-serial1` change (archived) that freed the console port. That
change gives the console an owner, `src/cli.cpp`, and makes it the port's only writer
while tasks run: a trace can no longer `print()` into `CLI_SERIAL` directly without
becoming a second writer, which `specs/console-cli/spec.md` forbids. The trace has to
reach the port through the CLI somehow — a new command that dumps a ring buffer the
`TaskMavlink` switch and the `TaskSerialWrite` drain fill, most likely — rather than
writing on its own.

That change's design also answers three of this entry's open questions directly,
since it had to answer them for `ps` and `free`:

- **Writing to the console cannot block the flight.** Answered: the console's own
  task runs at `PRIORITY_LOWEST`, and nothing else may write to the port while
  tasks run. A trace reusing that ownership inherits the same guarantee.
- **Several tasks writing at once interleave the output.** No longer applies as
  stated: with one owner and one writer, nothing else touches the port to
  interleave with. A trace has to get its data *to* the CLI (a queue, most likely,
  since `TaskMavlink` and `TaskSerialWrite` are not the CLI's task) rather than
  write the port itself.
- **Formatting cost.** Answered: `print()` and padding loops, not `snprintf` or
  `vfprintf` — measured at zero flash growth for the CLI's own four commands.

What it should trace, per message: direction (inbound/outbound), `msgid` — by name
if possible, not just the number —, source `sysid`/`compid` and length. The two
mandatory choke points already exist and are the natural places to hook it: the
`TaskMavlink` switch for what comes in and the `TaskSerialWrite` drain for what goes
out.

Points still to resolve before implementing:

- **How the profiles are separated.** A second `[env:...]` in `platformio.ini`
  inheriting from the current one and adding its `build_flags` is the idiomatic
  PlatformIO way. Careful: CI runs `pio run` with no `-e`, which builds *every*
  environment — so the debug profile is checked on every push too, which is
  desirable, but it must not become the one flashed to the board by default.
- **The trace cannot exist in release.** It has to be compiled out with `#ifdef`,
  not left behind a runtime `if`: the text strings and the formatting take flash and
  RAM, and neither is spare here. The release profile must produce exactly today's
  binary.
- **How the trace data reaches the CLI's task.** A bounded ring buffer the CLI
  reads on demand (a new command) rather than a stream the port pushes
  unsolicited, since `specs/console-cli/spec.md` requires the console emit
  nothing unless spoken to.
- **Stacks.** Formatting text consumes stack, and they are tight, between 96 and 256
  words. When enabling the debug profile the log high-water marks have to be checked
  again: this is exactly the case that triggers the slow blink of `src/hooks.cpp`.
- **What else goes into the debug profile.** A configurable detail level (headers
  only, or a hex dump), and whether it is reused for other subsystems' traces or
  stays MAVLink-only.
- Document in `README.md` and `CLAUDE.md` how to build and upload each profile.

### Answer the GCS messages that are ignored today

**Status:** proposed
**Scope:** `src/mavlink.cpp`

The `TaskMavlink` switch has several `case` branches that only `break`: `COMMAND_LONG`
(including `MAV_CMD_GET_HOME_POSITION`), `PARAM_REQUEST_LIST` and `REQUEST_DATA_STREAM`.
The GCS gives them up for lost and retries: MAVProxy sits waiting for a `COMMAND_ACK`
that never arrives. `openspec/changes/archive/2026-09-13-add-degraded-mode` answers one
specific command, `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN`, with a `COMMAND_ACK`, since it
needed that command to actually do something; every other branch here is unaffected.

The bare minimum is to always answer something. A `COMMAND_ACK` with
`MAV_RESULT_UNSUPPORTED` is an honest answer and stops the retry; silence is not.

**`AUTOPILOT_VERSION` (148), in response to `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES`
(520), is answered as of `answer-autopilot-version-requests`** — `capabilities` reports
only `MAV_PROTOCOL_CAPABILITY_MAVLINK2`, the one true capability this firmware has today.
The rest of this entry is still open.

To decide:

- Which commands are really supported and which are explicitly rejected.
- **Telemetry rates from the ground.** Today they are hard-wired in `TaskMavlink`'s
  schedule table (`fold-periodic-telemetry-into-mavlink-task`). Rather than
  implementing `REQUEST_DATA_STREAM`,
  which is deprecated, `MAV_CMD_SET_MESSAGE_INTERVAL` (511) with `MESSAGE_INTERVAL`
  (244) is the current mechanism. It matters over a narrow radio link, and even more
  so once *[Download the flight log over the MAVLink log protocol]* or *[Serve the SD
  card over MAVLink FTP]* lands and competes for it.
- The parameter protocol has its own entry:
  *[Implement the MAVLink parameter protocol]*.


### Emit `SYS_STATUS`

**Status:** proposed
**Scope:** `src/mavlink.cpp`, `include/SdRecord.h`

`SYS_STATUS` (1) is the most conspicuous absence in the current telemetry. It
carries the `onboard_control_sensors_present`, `_enabled` and `_health` bitmasks,
CPU load, battery voltage and percentage, and communication error counters. Every
GCS shows it front and centre; today the satellite sends none of it.

It fits with two things the firmware already has half done:

- The health bitmasks are the place to express "the SD card failed", "the RTC was
  lost", "the IMU does not answer".
  `openspec/changes/archive/2026-09-13-add-degraded-mode` (which consumed *Report the
  satellite's real state in the heartbeat*) already spends `custom_mode`'s four bytes
  on the reset reason, the boot phase and two fault counters, and its own design notes
  that a *continuing* indicator for a missing SD card has nowhere left to go in that
  field — `SYS_STATUS`'s sensor bitmap is exactly the candidate it points at without
  adopting it. Both entries share the same source: a centralised health state, which
  does not exist today.
- `errors_count1..4` is where to keep the count of sends dropped by a full queue,
  which are silently lost today. This used to name failed run-time allocations as the
  other candidate; there are none left to count, and the one that would matter now
  halts the board through the malloc-failed hook rather than returning to a caller
  that could tally it.

To decide: which subsystems are declared in `present`/`enabled` (the
`MAV_SYS_STATUS_SENSOR` enumeration has no entries for "SD card" or "RTC", so the
closest ones have to be chosen or it has to be accepted that some things are only
reported through `STATUSTEXT`), and at what rate it is emitted.


### Implement the MAVLink parameter protocol

**Status:** proposed
**Scope:** `src/mavlink.cpp`, `lib/` (storage), `platformio.ini`

`PARAM_REQUEST_LIST` (21) → `PARAM_VALUE` (22), plus `PARAM_REQUEST_READ` (20) and
`PARAM_SET` (23). For a satellite that cannot be reflashed, this adds more
capability than anything else on the list: it turns things that are hard-wired in
the code into ground-adjustable ones — battery thresholds, telemetry rates, the size
of the `lib/SdData` ring, and the magnetometer calibration offsets once *[Add the
GY-87 IMU]* lands.

Immediate side effect: MAVProxy stays at "waiting for parameters" until somebody
answers `PARAM_REQUEST_LIST`. Even with an empty list, it has to be answered.

To decide:

- **Where they persist.** The R4 has non-volatile data memory, and there is also the
  already-mounted SD card. Each option has its risk: the card can fail (see *[SD
  logging failure is silent]*) and leave the satellite without configuration.
- **What happens if a parameter is corrupted.** Compiled-in defaults and range
  validation on load, or whatever is there is accepted.
- **What is a parameter and what is not.** Everything adjustable from the ground is
  also everything breakable from the ground: a badly set threshold can leave the
  satellite useless.
- Sending the full list cannot monopolise the link or the write queue: it has to be
  chunked, not dumped as every `PARAM_VALUE` at once.



## To change

### `check_telemetry.py`'s `test_battery_status_every_2s` assumes the normal configuration

**Status:** defined
**Scope:** `test/test_hil/check_telemetry.py`

Found incidentally during `add-mavlink-housekeeping-telemetry`'s post-archive board
verification (2026-09-18): `pio test -e bench` failed this case —
`BATTERY_STATUS at 0.00 Hz, expected 0.50 Hz (seen 0 in 12.1s)` — because the board
was in the reduced configuration, where `sendBatteryStatus`'s schedule entry is
correctly disabled (`src/mavlink.cpp`). The test itself is unchanged by that change;
it has just never been run against a reduced board before. Unlike
`check_recovery.py`'s state-decode cases, it does not check the current
configuration first and self-skip (`NoLinkError`) when its assumption does not
hold — it should, the same way `check_housekeeping.py`'s cases already do (read the
next `HEARTBEAT`'s `system_status`).

### `memory-budget`'s queue requirements still describe the heap

**Status:** defined
**Scope:** `openspec/specs/memory-budget/spec.md`

`replace-messagepack-log-with-dataflash` moved the last queue off the FreeRTOS heap and
`configTOTAL_HEAP_SIZE` is `0x0`, with the allocator dropped from the image entirely. It
did **not** carry a spec delta for `memory-budget`, so that live capability still states
the model it replaced:

- *A declared queue depth is backed* requires the reserved memory to "account for the
  items that exist without sitting in a queue: the item a producer is holding when it
  discovers the queue is full, the item a consumer holds between taking it and releasing
  it, and one such item for every producer that can be doing this at the same instant."
  With items carried by value those are locals on each task's own stack, counted by the
  linker inside the stack that already exists. There is nothing to reserve.
- The same requirement ends "A producer whose item the queue did not accept SHALL release
  that item's memory." There is no memory to release, and nothing can release any: CI
  greps all of `src/` for the allocator by name.
- *Task and queue memory is accounted for at build time* has a scenario asserting that
  "the pool from which memory is obtained at run time is committed to queued items only".
  There is no such pool.

This is not cosmetic. `openspec/config.yaml`'s proposal rule now states the by-value
model, so the guidance injected into every new proposal and the canonical spec say
opposite things, and a proposal can satisfy one while violating the other. The rule
carries a warning pointing here, which is a signpost and not a fix.

Found by Copilot reviewing the planning-hygiene pull request that corrected the same drift
everywhere except here.

To decide: whether the requirements are rewritten around by-value queueing — depth times
item size in `.bss`, and a rejected send costing nothing — or whether the run-time-pool
requirements are removed outright as describing a facility the firmware no longer has. It
needs a change with a MODIFIED delta either way; a live spec is not hand-edited outside
one.

### Replace `arduino-libraries/SD` with `greiman/SdFat`

**Status:** defined
**Scope:** `platformio.ini`, `lib/SdData`, `test/test_libs/test_main.cpp`,
`ARCHITECTURE.md`

`arduino-libraries/SD` is a fork of an ancient SdFat and it is the reason this project
cannot do what every comparable project does. It has no `preAllocate()`, no `truncate()`,
no contiguous-write fast path, and one global 512 B cache block.

What that unlocks, in order of value:

- **`preAllocate()` reserves clusters in the FAT without writing a byte of data.** It is a
  metadata operation, cheap. A file that never grows never sets `F_FILE_DIR_DIRTY`, so its
  directory entry is **never touched in flight** — which **removes** the wear target rather
  than merely reducing it. `openspec/changes/archive/2026-09-22-size-the-log-ring-and-batch-its-flushes/`
  cuts the directory writes by about 36x by batching the flush; pre-allocation is what would
  take them to zero, and that is the whole remaining value of this entry. madflight pre-allocates 100 MB and calls `truncate()` on close to give back
  what it did not use.
- **It removes the rotation watchdog risk.** If the next file is already allocated there is
  no FAT walk on the flight path.
- Betaflight went further and wrote [asyncfatfs](https://github.com/thenickdude/asyncfatfs)
  for the same reason: it reserves the largest contiguous free region as a file and lends
  space from it, so "the FAT entries for the file need never be read".

**Start with a spike, not a swap.** RAM is the binding constraint and this is a new
dependency: measure SdFat's `.bss` and flash against the headroom `scripts/ram_budget.py`
reports (3184 B as of 2026-09-20) before committing to anything. SdFat is configurable
(`SdFatConfig.h`) and has a reduced mode, so the first question is what the smallest
useful configuration costs. If it does not fit, this entry closes as "does not fit" and
`openspec/changes/archive/2026-09-22-size-the-log-ring-and-batch-its-flushes/` stands on
its own, which it can: batching the flush gets about 36x without any new dependency, and
pre-allocation is what would take the directory writes to zero.

### Put the ring's position in the log data instead of `index.bin`

**Status:** defined
**Scope:** `lib/SdData`, `src/sdwrite.cpp`, `include/SdRecord.h`,
`openspec/specs/flight-log/spec.md`

`index.bin` is a second file that has to stay in agreement with the data, and it is
written from the rotation path with its own `open`/`seek`/`write`/`flush`/`close`. If it is
lost or torn the ring does not know where it is. It also evicts the single `SdVolume` cache
block every time it is touched.

ArduPilot's `AP_Logger_Block` — the backend for raw flash chips, with no filesystem at all
— does not have this piece, because the information lives in the data. Every page carries a
header with `FileNumber` and `FilePage`, and at startup `find_last_page()` runs a **binary
search** for where the pair stops increasing:

```c
while (top - bottom > 1) {
  look = (top + bottom) / 2;
  StartRead(look);
  look_hash = (int64_t)GetFileNumber() << 32 | df_FilePage;
  if (look_hash < bottom_hash) { top = look; }
  else { bottom = look; bottom_hash = look_hash; }
}
```

A new log is `FileNumber + 1`. The end of the ring is where the counter goes backwards.

Adapted to a ring of *files* rather than raw flash, the equivalent is a monotonically
increasing lap or session counter written into each file when it is opened — a record type
with its own `FMT`, so it stays `DFReader`-visible — and a boot that reads the four
counters and picks the highest. That satisfies the existing requirement (*The log occupies
a bounded amount of the card*: "The position within that set SHALL survive a power cycle")
without a side-car file that can desynchronise.

**That side-car argument is the whole case, and it is a thin one.** This entry first
claimed a second benefit — that a counter in the file is also how a reader finds the end of
a file written over on a later lap — and that benefit does not exist. Rotation deletes the
next slot before opening it, so a file never contains records from a previous lap and there
is no end to find. Do not pick this up expecting it to solve a parsing problem; it solves
exactly one thing, which is `index.bin` being a separate file that has to stay in agreement
with the data and can be lost or torn on its own.

Note also that ArduPilot's *file* backend does **not** do this — page headers exist because
raw flash has no filesystem, where there is genuinely nothing else to hold the position. So
this is a borrowed idea, not a copied one, and whether a side-car file is actually worse
than a counter in every file is the thing to settle before writing any code.

### Decide whether `SYSTEM_TIME` deserves 1 Hz

**Status:** proposed
**Scope:** `src/mavlink.cpp`, `openspec/specs/mavlink-link/`, `test/test_hil/`

The link emits `HEARTBEAT` and `SYSTEM_TIME` at 1 Hz and `BATTERY_STATUS` every 2 s.
Two of those three are where convention would put them, and one is not.

`HEARTBEAT` at 1 Hz is not a policy this firmware gets to choose — it is the
protocol's contract, and ground stations use its absence to declare the link dead.
`BATTERY_STATUS` every 2 s is in line with how autopilots rate battery telemetry, and
on a satellite the voltage moves over minutes anyway. `SYSTEM_TIME` at 1 Hz is the
outlier: conventionally it belongs to a slow stream group rather than the 1 Hz core,
nothing on the ground consumes it that often, and it costs about 24 B/s of the ~69 B/s
the link emits — roughly a third of the downlink, spent restating a clock that
advances predictably.

There is a counter-argument specific to this firmware, and it is why this is a
question rather than a defect. The RTC may be absent — the reduced configuration runs
on ticks since boot — and it is the ground that sets the clock through inbound
`SYSTEM_TIME` and `TIMESYNC`. Emitting it often is how the ground notices the clock is
unset or wrong. Whether noticing that needs 1 Hz, or 0.2 Hz would do, is the decision.

Bandwidth is not the argument today: at `LINK_BAUD` 57600 the whole telemetry set is
about 1.2% duty cycle. Decide this against whatever the real radio's budget and pass
structure turn out to be, not against the bench UART.

**Changing any of these rates alters the MAVLink surface.** It needs a spec delta —
`openspec/specs/mavlink-link/spec.md` states the three rates twice, in the scenario at
lines 20-21 and again at line 90 — and it touches whichever HIL cases assert them. It
was deliberately not folded into
`openspec/changes/archive/2026-09-18-fold-periodic-telemetry-into-mavlink-task/`, whose
value rested on being invisible from the ground. That change has landed, so the rate is
now one number in its schedule table in `src/mavlink.cpp`.

Related: `openspec/changes/archive/2026-09-19-improve-clock-synchronisation/` covered the
quality of the timestamp — resolution, provenance and time base — not how often it is
sent. The two are independent, so that change landing did not answer this question.

### Fix the pointer arithmetic in the unknown-message `STATUSTEXT`

**Status:** defined
**Scope:** `src/mavlink.cpp`

In the `default` branch of the `TaskMavlink` switch, `src/mavlink.cpp:222`:

```cpp
sendStatusText("Mensaje recibido con ID desconocido: " + msg->msgid, MAV_SEVERITY_WARNING);
```

`sendStatusText` takes a `const char*`, so there is no string concatenation here:
the `+` is **pointer arithmetic**. The literal is 37 characters long and the pointer
advances `msg->msgid` bytes over it, so that:

- with a `msgid` below 37, a fragment of the tail of the literal is sent, without
  the number it was meant to show;
- with a larger `msgid` — the usual case, MAVLink identifiers reach the hundreds —
  it reads **past the literal** and transmits arbitrary flash bytes to the ground
  until it hits a `\0`.

It fires with any message not covered by the switch, which is exactly what the
`default` branch exists for.

The fix is to format the number into a bounded local buffer (`snprintf` over a local
`char[]`) before calling `sendStatusText`, bearing in mind that `STATUSTEXT` cuts
the text at 50 characters and that `TaskMavlink`'s stack is 256 words.

The message text should also become English along with the rest of the repository.

Worth reviewing together with *[Debug and release builds...]*: if the protocol trace
ends up printing the `msgid` to the console, formatting the number should be solved
once and not in two places.

### The stack overflow hook hangs before it warns

**Status:** defined
**Scope:** `src/hooks.cpp`, `ARCHITECTURE.md`

`vApplicationStackOverflowHook()` calls `taskDISABLE_INTERRUPTS()` and then
`while (!Serial) {}`. The USB CDC needs interrupts to enumerate: with no host
connected — that is, in flight — that loop never ends and the board is dead
**without blinking**. The `delay(2000)` that follows has the same problem, because
it depends on the tick, which has also just lost its interrupts.

So the documented diagnostic only works if a PC was already plugged in, which is
exactly the case where it is least needed.

The blink is also 2000 ms on and 2000 ms off: a 4 s period, 0.25 Hz.

To decide: whether the visual warning should come first and the serial message
after (only if the port was already up), or whether the blink should move to direct
pin register manipulation and a busy-wait delay, depending on nothing that needs
interrupts. `ARCHITECTURE.md` describes this hook as trapping an overflow into a slow
blink; once the behaviour is fixed, state the real period there.

Overlaps with `openspec/changes/archive/2026-09-13-add-degraded-mode` (formerly *Add a
watchdog* and *`setup()` asserts on the RTC before the console exists*, both consumed
by that change): with a watchdog, sitting here blinking forever stops being the obvious
answer to an overflow, and that change's own review (`review.md` finding 9)
already names the fix this entry describes — writing the phase marker first and
resetting immediately, no blink — as not yet folded in.


### SD logging failure is silent

**Status:** proposed
**Scope:** `lib/SdData`, `src/sdwrite.cpp`, `src/mavlink.cpp`

If `SD.open` fails in `SdData::begin()`, the object is left with no file and
`write()` returns without doing anything, indefinitely. Neither `begin()` nor
`write()` return anything, and `TaskSdWrite` cannot tell "stored" from "thrown
away".

Result: the entire mission log can be lost without a single warning over the link.
`setup()` reports a missing card at boot
(`openspec/changes/archive/2026-09-13-add-degraded-mode` turned the old
`configASSERT(SD.begin(9))` into a degradation), but that only
covers boot; a card that fails or is unmounted later still goes unnoticed.

To decide: having `begin()`/`write()` return a result and `TaskSdWrite` propagate
it; and how the ground finds out — a `STATUSTEXT`, a field in the heartbeat (whose
`custom_mode` bytes `add-degraded-mode` already spent on the boot-time state — see
*Emit `SYS_STATUS`* above), or both. Be careful not to flood the link by repeating
the warning at 1 Hz.

Note that `openspec/changes/make-sddata-begin-idempotent/` is touching `begin()` now and
deliberately does **not** change its `void` return — it says so in its own Non-Goals. So
the signature question is still entirely open here, and that change also gives the error
path a new reason to exist: recovering from a card failure means reopening the log, which
is the case that change makes work.


### `TaskSerialRead` polls the port instead of waiting

**Status:** proposed
**Scope:** `src/serial.cpp`

The `TaskSerialRead` loop drains whatever is available and then does
`vTaskDelay(10 ms)`. At 57600 baud that is ~57 bytes per cycle against a typical
64-byte receive buffer: the margin is minimal, and at higher speeds bytes are lost
silently — which from the ground looks like intermittent corrupt MAVLink frames, the
hardest kind of fault to diagnose.

On top of that the task is `PRIORITY_HIGHEST`, so it wakes a hundred times a second
even when there is nothing to read.

To decide: whether it moves to a genuinely blocking wait (a task notification from
the receive path, or a semaphore) or the polling period is simply shortened. The
former is the right answer but depends on what the port chosen in the
`move-mavlink-link-to-serial1` change exposes, so it is better resolved after it.
Note that the premise above needs re-checking: `SERIAL_BUFFER_SIZE` on this core is
512, not 64, and the ring is filled by the receive ISR, so a late task loses nothing.


### Have Dependabot watch the PlatformIO libraries too

**Status:** defined
**Scope:** `.github/dependabot.yml`

`.github/dependabot.yml` only declares the `github-actions` ecosystem. The
`lib_deps` dependencies in `platformio.ini` — MAVLink, ArduinoJson, RTClib, Adafruit
BusIO, SD, SolarCharger — are watched by nobody: they get updated when somebody
remembers.

Dependabot has no PlatformIO ecosystem, so the alternative has to be decided:
pinning versions in `lib_deps` and reviewing them by hand periodically, or a
scheduled job that checks for new versions and opens the notice.

Tied to this: `lib_deps` pins no library version at all. Reproducing a build from
six months ago is not possible today, and a breaking update to any of the six lands
in the next `pio run` with no warning.

### The toolchain and uploader are x86_64-only, and Rosetta ends with macOS 28

**Status:** proposed
**Scope:** `platformio.ini`, upstream — no code of this project

Neither of the two binaries this project needs to build and flash runs natively on an
Apple Silicon Mac, and both are reached only through Rosetta:

- **The compiler.** `platform-renesas-ra` pins `toolchain-gccarmnoneeabi` to
  `~1.70201.0` (GCC 7.2.1). That package's manifest declares
  `"system": ["darwin_x86_64", "darwin_arm64"]` but ships **one** Mach-O x86_64 binary
  for both, so PlatformIO installs it on Apple Silicon without complaint and every
  compile then fails with `Bad CPU type in executable`.
- **The uploader.** `tool-dfuutil-arduino@1.11.0` — the only published version — has
  exactly the same false claim and the same single x86_64 file.

**This has been investigated and measured; the findings are worth not rediscovering.**

A native arm64 alternative exists for the compiler and works:
`platformio/toolchain-gccarmnoneeabi@1.120301.0` (GCC 12.3.1) has a genuine
`darwin_arm64` artifact, set through `platform_packages` in `[env]`. It builds all
three environments clean, `pio check` stays at 13 LOW findings, no new `-Wall -Wextra`
warning appears in `src/`, `lib/` or `include/`, and RAM **improves**: `committed
31176 B of 32768`, `headroom 1592 B` against 1468 today. Do not reach for the newest
version instead: `1.140201.0` (GCC 14.2.1) fails to compile the Arduino core's
vendored TinyUSB (`cores/arduino/tinyusb/rusb2/dcd_rusb2.c:289` — `TU_ASSERT` returns a
value from a `void` function, which GCC 14 rejects as a hard error that `-w` does not
suppress), because `framework-arduinorenesas-uno` publishes only version `1.6.0` and
its own `installed.json` records Arduino building it with `arm-none-eabi-gcc
7-2017q4` — upstream has never compiled it with anything newer.

The uploader has **no working answer**. `platform_packages` cannot reach it:
`builder/main.py:150` resolves it by the hardcoded name `tool-dfuutil-arduino`. An
`upload_command` override does work as a mechanism and is CI-safe (only the upload
target evaluates it), and `platformio/tool-dfuutil@1.11.241029` is a genuine arm64
build — but that binary cannot claim the device on macOS: with the board confirmed in
DFU mode by `ioreg` (`Santiago DFU`, `0x2341:0x0369`), `dfu-util -l` lists nothing. It
also rejects `-Q`, which is an Arduino-fork flag absent upstream. A Homebrew
`dfu-util` links its own libusb and might succeed; untested.

Where each piece is maintained, since no manifest points at the right place — the
`tool-dfuutil-arduino` manifest names upstream SourceForge while shipping Arduino's
patched fork:

| Piece | Repository |
|---|---|
| Arduino core (the TinyUSB that GCC 14 rejects) | `github.com/arduino/ArduinoCore-renesas` |
| PlatformIO platform (the hardcoded uploader name) | `github.com/platformio/platform-renesas-ra` |
| Arduino's `dfu-util` fork (no Apple Silicon target) | `github.com/arduino/dfu-utils-cross` |
| PlatformIO picking x86_64 when Rosetta is absent | `platformio/platformio-core` issue 5393, open since 2026-03 |

**The deadline.** Rosetta remains available through macOS 27 — it is uninstalled by the
upgrade and restored with `softwareupdate --install-rosetta` — but macOS 28 removes it
except for certain legacy games. Before then this project needs the native compiler
above, a working uploader, or containerised builds. The compiler half is solved and
verified; the uploader half is not, and it is the one that blocks every `[board]` step.

Whoever picks this up: moving the compiler re-bases every RAM figure in
`ARCHITECTURE.md` and in any change then in flight, so do it when nothing else is
mid-flight, and re-read the high-water marks on the board — a new compiler changes
stack frame sizes, and the tightest margin in the fleet is `UartRead` at 41 words
free of 96.

### The CI PlatformIO cache key hashes a file that does not exist

**Status:** defined
**Scope:** `.github/workflows/main.yml`

`main.yml`'s `Cache PlatformIO` step keyed `~/.platformio` on
`hashFiles('**/lockfiles')`, and no file named `lockfiles` has ever existed in this
repository. `hashFiles` returns an empty string for no match, so the key was the
constant `Linux-`: it hit on every run, and because `actions/cache` only writes a new
entry on a miss, it was never refreshed either. Harmless while the toolchain never
changed — and silently wrong the moment it does, since the restored cache would keep
serving the old one.

Already fixed to `hashFiles('platformio.ini')` with `restore-keys: ${{ runner.os }}-`,
matching the shape the pip cache block above it uses. This entry exists because that
fix landed without a change behind it, found while investigating the toolchain, and
because the same class of mistake is worth checking for in any cache key added later:
confirm the path `hashFiles` is given actually matches something.

### Review the contents of the messages already emitted

**Status:** defined
**Scope:** `src/mavlink.cpp`

The four messages the satellite emits today go out with empty, constant or outright
misleading fields. No new hardware is needed to fix a good part of it:

- **`STATUSTEXT` is misused, not just badly formatted.** Beyond the bug in *[Fix the
  pointer arithmetic...]*, the design problem is that the `default` branch of the switch
  answers the ground with a text message **for every inbound message not covered**. A
  talkative GCS continuously sends things the switch does not cover
  (`MISSION_REQUEST_LIST`, `PARAM_REQUEST_READ`, `MISSION_COUNT`...), so the satellite
  spends its time flooding a narrow link with complaints. Take it out of there and
  reserve `STATUSTEXT` for what deserves a warning: an SD failure, and — since
  `openspec/changes/archive/2026-09-13-add-degraded-mode` — the cause of the last reset,
  already emitted once per boot.
- **`BATTERY_STATUS` goes out nearly empty:** `current_battery`, `current_consumed`
  and `energy_consumed` at `-1`, `time_remaining` at `0`, `temperature` at `INT16_MAX`
  and `charge_state` at `MAV_BATTERY_CHARGE_STATE_UNDEFINED`. Two of them can be filled
  with no additional hardware as soon as *[Add the GY-87 IMU]* (temperature) and
  *[Detect when the battery is charging]* (charge state) land. `time_remaining` requires
  measuring current.
- **`HEARTBEAT` used to declare things that were not so.**
  `openspec/changes/archive/2026-09-13-add-degraded-mode` (which consumed *Report the
  satellite's real state in the heartbeat*) makes `custom_mode` carry the reset reason,
  the boot phase and both fault counters, and drops `MAV_MODE_FLAG_AUTO_ENABLED` in the
  reduced configuration. `MAV_MODE_FLAG_SAFETY_ARMED` stays fixed regardless — that
  remains a separate decision.
- **`MAV_TYPE_ROCKET` is debatable.** There is no `MAV_TYPE_SATELLITE`, but
  `MAV_TYPE_GENERIC` describes a CubeSat better than a rocket does, and it changes how
  the GCS draws it. Worth deciding soon: `ARCHITECTURE.md` fixes it as the bus identity
  and the more code assumes it, the more it costs to change.
- **`SYSTEM_TIME` at 1 Hz is a lot** for something that hardly ever changes in an
  interesting way. If `MAV_CMD_SET_MESSAGE_INTERVAL` lands in *[Answer the GCS messages
  that are ignored today]*, this solves itself.


### Minor leftovers cleanup

**Status:** defined
**Scope:** `src/mavlink.cpp`, `src/hooks.cpp`, `test/test_libs/test_main.cpp`

Small, unrelated things worth getting out of the way in one go:

- `src/mavlink.cpp` declares `extern RTC_DS1307 rtc;`, a global that exists nowhere.
  It does not fail to link only because nobody uses it.
- `src/mavlink.cpp` declares `mavlink_command_long_t command;` inside a `case`
  with no braces of its own, which puts a declaration in the scope of the rest of
  the switch. It compiles because it has no initialiser. Note that cppcheck does
  **not** flag it: the `add-static-analysis-to-ci` change measured what the checker
  actually reports, and this is not in it.
- A space is missing in `"Overflow on" + String(pcTaskName)` in `src/hooks.cpp`.

Two items left this list on 2026-09-20 without anyone doing them:
`src/logger.cpp`'s GNU label initialiser (`unixtime: ...`) and the test's
`StaticJsonDocument`. `replace-messagepack-log-with-dataflash` rewrote the logger and
dropped ArduinoJson as a dependency entirely, which also took cppcheck's three
`unusedLabel` findings in `src/logger.cpp` with it.

A third left on 2026-09-21, this one actually done: `TEST_FILE_SIZE_MB` is now
`TEST_FILE_SIZE_BYTES`, renamed by
`openspec/changes/archive/2026-09-22-size-the-log-ring-and-batch-its-flushes/` because that change had to
alter the value anyway — and it had to raise it from 1024 to **6144**, a value constrained
from both sides: larger than the 4 KiB flush interval, or a rotation always beats it and
`close()` syncs so the suite cannot exercise batching at all; and not a multiple of it, or
the rotation lands straight after a sync with no unsynced tail to lose.

### The tests do not link FreeRTOS, so nothing covers the tasks

**Status:** defined
**Scope:** `platformio.ini`, `test/test_libs/test_main.cpp`, `CLAUDE.md`, `ARCHITECTURE.md`

`test_build_src` defaults to `False` in PlatformIO, and `platformio.ini` does not
set it. `src/` is therefore not compiled into the test binary: no `main.cpp`, no
`xTaskCreate`, no `vTaskStartScheduler`. The five cases link `Battery`,
`SystemTime` and `SdData` against the Arduino core and nothing else.

The section sizes confirm it. The application firmware has a `.bss` of 19876 bytes,
which contains `ucHeap` and every task stack; the Unity binary built from the same tree
has a `.bss` of 4608 bytes, too small to hold the 6144-byte heap array, and contains
neither `vTaskStartScheduler` nor `xTaskCreateStatic` at all. FreeRTOS is simply not in
the test binary.

Note also that `test_build_src` governs `pio test`, not `pio run`: `pio run -e libs`
builds the application and does carry the static storage.

The consequence is that `pio test` covers the libraries in isolation and **cannot
observe the RTOS at all**: not a stack size, not a queue depth, not the pointer
ownership protocol, not a high-water mark, not the overflow hook. Those are exactly
the invariants that are easiest to break silently, and the ones `CLAUDE.md` and
`openspec/config.yaml` currently imply a test run would catch. A green `pio test`
after changing a task body means nothing about that task body.

To decide:

- Whether `test_build_src = yes` is the answer. It would pull `src/main.cpp` into
  the test binary, and with it a second `setup()` competing with Unity's, so it
  needs a guard (`#ifndef PIO_UNIT_TESTING` around the task creation, or moving the
  scheduler start out of `setup()`).
- Whether the RTOS is worth testing on-target at all, or whether the honest fix is
  to state the gap in `CLAUDE.md` and `ARCHITECTURE.md` and keep relying on the SD
  log's high-water marks as the only evidence that the stacks fit.
- Whether a separate test environment is better than one binary: a `[env:...]` with
  its own `test_build_src` would keep the current library tests fast and let a
  second suite exercise the task graph.

Until this is settled, the guidance in `CLAUDE.md` and the `tasks` rule in
`openspec/config.yaml` overstate what a test run proves for a change to a task
body, and both should be narrowed to `lib/`.

### Recover the bricked board

**Status:** defined
**Scope:** hardware, `test/test_hil/`

The board has been unreachable since a firmware built from the `add-console-cli`
change was flashed 72 bytes short of the FreeRTOS heap. It does not enumerate as a
USB CDC device, so the 1200-baud touch that puts the bootloader into DFU cannot be
delivered: that path runs entirely inside `tud_task()`, which this core services from
the USB interrupt, and both plausible death paths mask interrupts.

Recovery therefore needs physical access — a double tap of RESET puts the bootloader
in DFU regardless of what the sketch is doing, after which `pio run -t upload`
restores a working firmware.

Until this is done, every step marked **[board]** in an OpenSpec change is blocked,
and `pio test` reports every HIL case as `IGNORE` and exits 0 — so a green run proves
nothing.

To decide:

- Whether anything should be added so this cannot recur silently. `VBTBKR[4..511]`,
  the RA4M1's battery-backed registers, survive any reset and are free once the
  bootloader's own 32-bit double-tap magic at `VBTBKR[0..3]` is left alone. That boot
  counter is now `openspec/changes/archive/2026-09-13-add-degraded-mode`, which selects
  a reduced task set on repeated failure but deliberately does not park the board in DFU
  — see that change's design for why. It is not part of recovering *this* board: the
  change still needs the board reachable before it can be flashed.
- Whether the SWD pads are worth wiring for a probe, which would make this recoverable
  without the enclosure open.


### Gate the watchdog refresh on the link still emitting

**Status:** proposed
**Scope:** `src/serial.cpp`, `src/mavlink.cpp`, `src/hooks.cpp`, `include/`

The watchdog added by the `add-degraded-mode` change is refreshed from
`vApplicationIdleHook()`, which detects **starvation** — a task running and not
yielding. It cannot detect **silence**, because a firmware in which every task is
legitimately blocked is indistinguishable from an idle one: the idle task runs, the
refresh happens, and the watchdog is satisfied.

That failure is reachable. If the producers into `serialWriteQueue` all stopped,
`TaskSerialWrite` would wait on `xQueueReceive(..., portMAX_DELAY)` for ever,
`TaskMavlink` likewise on its own queue, and `TaskSerialRead` would poll every 10 ms and
find nothing. All four tasks correct, all four yielding, satellite mute — and the
watchdog reporting it healthy. **The current mechanism proves the firmware is running,
not that the satellite is working.**

The gate is to refresh only when a frame has recently left the link:

```c
// src/serial.cpp, after LINK_SERIAL.write(buf, len) -- UART::write() busy-waits
// until the frame is on the wire, so returning means the bytes are out
linkFramesSent++;

// src/hooks.cpp, in the idle hook
// refresh only while that counter keeps advancing
```

Counting anywhere earlier proves less, and the difference matters:

| Where | What it proves |
|---|---|
| In `TaskMavlink`'s schedule | that `TaskMavlink` lives — and `sendHeartbeat()` ignores whether `xQueueSend` succeeded, so it lives happily while nothing drains the queue |
| After `xQueueSend` | that the queue accepted it, not that anyone drains it |
| After `LINK_SERIAL.write()` | that it left the satellite |

`TaskMavlink`'s schedule (`fold-periodic-telemetry-into-mavlink-task` folded this in
from `TaskHeartbeat`) fires `HEARTBEAT` and `SYSTEM_TIME` every 1 s, 500 ms apart, and
stays in the reduced task set, so at least two frames a second leave in any working
configuration. The staleness threshold follows from that rather than being invented.

**What this still would not cover, and the reason is in the specs.** Outbound liveness
is observable; inbound liveness is not, because silence on the inbound link is the
normal flight condition — `openspec/specs/mavlink-link/spec.md` requires that a board
powered with nothing attached reaches its steady-state cadence. Requiring inbound
traffic would reset the satellite on every gap between passes.

But the two inbound tasks differ, and only one of them is genuinely unobservable:

- `TaskSerialRead` polls unconditionally every 10 ms whether or not bytes arrive, so a
  counter in its loop would prove it alive with nothing attached.
- `TaskMavlink` blocks on `xQueueReceive(..., portMAX_DELAY)`, so with no traffic it
  never wakes and its idleness is indistinguishable from its death.

That is the worst one to lose. `TaskMavlink` is the task the reduced configuration keeps
precisely so the ground can command an exit: the one task that must not die is the one
whose death cannot be seen. Replacing `portMAX_DELAY` with a bounded timeout — a second,
say — and counting each wake makes it observable, at the cost of one wake per second in a
task that currently costs nothing when idle.

To decide:

- **The staleness threshold**, against the ~2 Hz floor, and whether it is the same in the
  reduced configuration.
- **Whether `TaskMavlink`'s timeout belongs here or on its own.** It changes a task body,
  so it needs a board and a look at the high-water mark.
- **Whether three counters is one too many.** Outbound frames, `TaskSerialRead` polls and
  `TaskMavlink` wakes would each be a `volatile uint32_t` with one writer and one reader —
  cheap individually, but it is three pieces of cross-task state in a firmware whose
  freedom from mutexes rests on single ownership, and that argument should be made
  deliberately rather than by accretion.


### Record which scenario each HIL case covers

**Status:** defined
**Scope:** `test/test_hil/check_*.py`, `test/test_hil/README.md`, `scripts/` (new check)

The HIL suite claims a coverage it does not record, and the claim was false in three
places until it was removed from `CLAUDE.md` and `test/test_hil/README.md`. Measured on
this tree:

- Nine `test_*` cases across four modules — `check_clock.py` 1, `check_silence.py` 1,
  `check_timesync.py` 1, `check_telemetry.py` 6 — against **eight** `#### Scenario:`
  headings in `openspec/specs/mavlink-link/spec.md`, so no one-to-one mapping is even
  arithmetically possible.
- The six cases in `check_telemetry.py`, two thirds of the suite, name no scenario at all.
- `check_clock.py` says it covers the scenario *"inbound SYSTEM_TIME and TIMESYNC sent on
  that port are acted upon"*. That text appears nowhere in the live spec — it is a
  requirement phrasing that has since been rewritten.
- `check_timesync.py` says "the scenario about TIMESYNC" without naming one.

What to do: give each case a machine-readable declaration of the requirement and scenario
it covers, rather than prose in a docstring, and add a check under `scripts/` that fails
when a case names a scenario that does not exist, when a scenario has no case and no
`[board]` step, or when the counts disagree. It needs no hardware — it compares text
files — so it belongs beside `scripts/ram_budget.py` in CI, where a wrong claim lands on
the author's desk.

Two reasons this is worth doing before the next change touches the link. It is the
prerequisite for `verify.md`'s coverage section to mean anything for `mavlink-link`, which
is the one capability with a live spec and a real suite. And the failure mode it prevents
is the one already demonstrated: every part of the false claim was checkable at any time
by anyone, for months, and nothing was positioned to look.

### Finish verifying what improve-clock-synchronisation could not

**Status:** defined
**Scope:** `test/test_hil/check_clock.py`, `test/test_libs/test_main.cpp`, `src/mavlink.cpp`

`openspec/changes/archive/*-improve-clock-synchronisation/` shipped with seven of its 31
tasks unticked, none of them for want of work: each is blocked on hardware this bench
cannot present. They are recorded here because the change directory stops being read once
it is archived, and unverified work that belongs to nobody is what this backlog is for.

- **The `survived` clock source has never executed** (that change's tasks 1.2, 3.5, 5.5).
  Observing it needs the boot report after a reset, which over USB is impossible for the
  reason the entry below this one describes. The HIL case exists and self-skips:
  `HIL_CLOCK_RESET=1` with `HIL_PORT` pointed at a USB-TTL adapter on D0/D1 closes it,
  and it also asserts the `ds1307` → `ground` promotion, which is the only place in that
  suite starting from a known origin. Until then the `system-clock` requirement *A clock
  set from the ground outlives a reset* is intent rather than demonstrated behaviour.
- **The no-clock configuration cannot be entered at all** (tasks 3.4, 5.4). The DS1307 is
  not disconnectable on this assembly and `setup()` calls `systemTime.begin()`
  unconditionally, reduced configuration included, so nothing can exercise origin `none`
  or the `system-clock` requirement *An unknown clock is reported as unknown*. This is
  the third time the same obstacle has blocked a task — `add-degraded-mode`'s 6.4 became
  part of that change, and its 6.6 is still open above for the same reason. Worth deciding
  once whether the board gets a way to present an absent DS1307, because three entries now
  wait on it.
- **The DS1307 reconciliation interval is a placeholder** (tasks 1.3, 3.6).
  `kClockReseedIntervalMs` in `src/mavlink.cpp` is six hours, chosen conservatively and
  marked as such in the code, because the drift it should be derived from was never
  measured. `test_report_internal_versus_ds1307_drift` prints both clocks and is meant to
  be run twice at least an hour apart; only the t0 reading was taken (`difference=0 s`,
  which is what seeding them together gives). Each run is `pio test -e libs`, destructive
  to the card. That figure also feeds the sub-clock entry below.

### The boot `STATUSTEXT` cannot be observed over USB after a reset

**Status:** proposed
**Scope:** `src/mavlink.cpp`, `test/test_hil/`

`sendBootStatusText()` and the clock report beside it are emitted once, from the top of
`TaskMavlink`. Over the USB CDC port that makes them unobservable across any reset: the
port drops when the board resets (measured — the host's handle fails with `Errno 6,
Device not configured` within 100 ms of the reboot command being acknowledged), and by the
time it has re-enumerated and a ground station has reopened it, the texts have already
been written into a port with no host attached, where they are discarded. The firmware is
right not to wait — *[No task waits for the link port to become ready]* is a requirement —
so the text is simply gone.

It does not matter for the reset reason, because that also rides `custom_mode` in every
heartbeat, continuously, which is exactly why `add-degraded-mode` put it there. It does
matter for anything whose only channel is the boot text. Over the **UART** link there is
no such window, since a USB-TTL adapter stays enumerated on the host while the board
resets, which is why this went unnoticed.

Found while trying to close `improve-clock-synchronisation`'s reset-survival measurement,
which needs the boot report and therefore cannot be closed over USB. That change accepted
the limitation rather than working around it (its tasks 1.2 and 5.5 are marked as needing
an adapter on D0/D1), so the `survived` clock source ships unexecuted until either an
adapter is attached or this is fixed.

To decide: whether to re-emit the boot texts a bounded number of times early after boot
(cheap, no new surface, but ad hoc), to answer them on request (needs a command that does
not exist), or to accept that boot-time facts need the UART and say so in
`test/test_hil/README.md`.

### Select the RA4M1 sub-clock for the internal RTC, if the crystal is populated

**Status:** proposed
**Scope:** `platformio.ini`, `ARCHITECTURE.md`

The Arduino core leaves `RTC_CLOCK_SOURCE` at `RTC_CLOCK_SOURCE_LOCO` — an on-chip RC
oscillator — behind an `#ifndef` (`libraries/RTC/src/RTC.cpp`), and every variant's BSP
config declares `BSP_CLOCK_CFG_SUBCLOCK_POPULATED (1)`. `-D RTC_CLOCK_SOURCE=RTC_CLOCK_SOURCE_SUBCLK`
in `build_flags` would therefore select the 32.768 kHz crystal instead, making the internal
RTC a far better keeper than it is today and changing which clock deserves to win when the
two disagree (`ARCHITECTURE.md` §5.3).

**One half is measured, one is not.** `improve-clock-synchronisation` task 1.4 confirmed
that project `build_flags` **do** reach `RTC.cpp`'s compilation unit — its line carries
`-DWDT_TIMEOUT_MS=1398` and `-DconfigTOTAL_HEAP_SIZE=0x200` — so the override would be
honoured. What is **not** established is whether the UNO R4 Minima physically populates
the crystal. The BSP declaring it is not the schematic having it, and selecting `SUBCLK`
without the part gives a **stopped** clock, which is a worse failure than a drifting one.
Answer that from the schematic before touching the flag.

The drift figure that would justify it is also still missing:
`improve-clock-synchronisation` left a Unity case
(`test_report_internal_versus_ds1307_drift`) that prints both clocks, meant to be run
twice at least an hour apart, and that measurement was not taken. Until it is, the 6-hour
DS1307 re-seed interval in `TaskMavlink` is a conservative placeholder, marked as such in
the code, not a measured value.


## Done

### Write the architecture document

**Status:** done
**Scope:** `ARCHITECTURE.md` (new), `README.md`, `CLAUDE.md`

The repository has no architecture documentation for people: `README.md` is a
handful of lines and the MAVProxy command, and the only thing describing the design
is the *Architecture* section of `CLAUDE.md`, written for Claude Code. Anyone new to
the project — or the author himself six months from now — has nowhere to see why the
firmware is split the way it is.

Content it should cover:

- The task and queue model: `src/main.cpp` as the only place where they are created,
  and each task in its own translation unit reaching the shared objects through
  `extern`.
- The memory ownership protocol, which is the rule most easily broken: queues carry
  heap pointers, the producer frees if `xQueueSend` does not return `pdPASS`, the
  consumer frees after use.
- The three pipelines — serial ↔ MAVLink, housekeeping log, time — and which file
  owns which resource (`src/serial.cpp` the UART, `src/sdwrite.cpp` the card).
- The constraints that explain the code as it stands: stacks in words and tight,
  priorities from `include/Priority.h`, fixed wiring, `configASSERT` hanging the
  board instead of degrading.
- A diagram of the pipelines and the queues. Mermaid renders on GitHub and is
  versioned as text.

Decisions taken (2026-09-08), which were open when this entry was written:

- **Language.** English, consistent with `README.md`, `CLAUDE.md` and the code.
  `TODO.md` was rewritten from Spanish to English at the same time so the whole
  repository speaks one language.
- **Single source of truth.** `ARCHITECTURE.md` owns the design. The *Architecture*
  section of `CLAUDE.md` is replaced by a pointer to it, and `CLAUDE.md` keeps only
  what is specific to Claude Code: commands, environment and conventions when
  editing.
- **Depth.** Current state only, no history of past decisions and no ADR directory.
  Decisions and constraints age well; enumerating functions and signatures ages
  badly and is already in the code.
- **Diagram first.** The document opens with the whole-system Mermaid diagram and
  then descends into detail.
- **Discrepancies.** Anything found in the code that does not add up while writing
  the document becomes a `TODO.md` entry rather than a "known gaps" section, so the
  architecture document describes the design and not its current defects.
- `README.md` grows into a real front page — hardware list, quick start, links —
  and the architecture document keeps the design.
