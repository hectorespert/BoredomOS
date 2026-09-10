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

### Add the GY-87 IMU

**Status:** proposed
**Scope:** `src/sensors.cpp` (new), `src/main.cpp`, `include/Data.h`,
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
- **Which libraries.** Each chip has its own and not all of them are lightweight;
  with 8 KB of FreeRTOS heap the cost is worth looking at before adding them to
  `lib_deps`.
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
- Extend `include/Data.h` and `src/sdwrite.cpp`. These are quite a few new fields:
  review how much each MessagePack record grows and what that does to the rotation
  rate of the `lib/SdData` ring.
- With three chips on the bus, I2C access is no longer exclusive to the RTC. How it
  is serialised against `lib/SystemTime` has to be decided, the same way the SD card
  is against `TaskSdWrite`.
- Checks in `test/test_main.cpp`, which runs against real hardware.


### Add a temperature sensor

**Status:** proposed
**Scope:** `src/sensors.cpp` (new), `src/main.cpp`, `include/Data.h`,
`src/logger.cpp`, `src/sdwrite.cpp`, `src/mavlink.cpp`, `platformio.ini`

Measure the on-board temperature and expose it through the two paths that already
exist: the housekeeping log on the SD card and the MAVLink telemetry to the ground.
It is critical data for a CubeSat: the LiPo and the SD card have a narrow operating
range and today there is no way to know at what temperature the board is flying.

To decide before implementing:

- **Which sensor.** If the IMU of *[Add the GY-87 IMU]* lands, the module already
  brings two temperature sources — the BMP180 and the MPU-6050 die — and this entry
  reduces to exposing that value, with no extra hardware or dependencies. The
  alternatives are the RA4M1 internal sensor, which needs nothing but measures the
  MCU die rather than the environment, or a dedicated I2C part. Better decided
  **after** the GY-87.
- **How many measurement points.** A single sensor, or several (battery, exterior)
  changes the shape of the data in `Data`.
- **Rate and caching.** `lib/Battery` caches for 125 ms; temperature changes far
  more slowly, so sampling can be much slower than the 1 Hz of `src/logger.cpp`.

Implementation points:

- `src/main.cpp:44` already declares `[[noreturn]] extern void TaskSensors(...)`
  with no implementation and no `xTaskCreate`: that is the reserved slot for this.
  `src/sensors.cpp` has to be created and given a priority from
  `include/Priority.h` and a stack size in words.
- Add the field to `include/Data.h`, fill it in `src/logger.cpp` and dump it in
  `src/sdwrite.cpp`. Touching `Data` changes the `.mpk` schema: decide whether old
  files remain readable.
- If the reading is not taken by `TaskLogger` itself, the value has to reach it
  without breaking the queue protocol: **heap pointers, `vPortFree` if `xQueueSend`
  does not return `pdPASS`, and the consumer frees**.
- Outbound MAVLink message: pick a standard one (`SCALED_PRESSURE.temperature` in
  centidegrees, or `HYGROMETER_SENSOR`) and emit it with the same identity triple as
  the rest: system `1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.
- Add the check to `test/test_main.cpp`, which only runs on real hardware. If the
  sensor is initialised with `configASSERT` in `setup()`, its absence will hang the
  board just as the RTC and the SD card do today.
- Watch the high-water marks after adding the task: the RAM margin is thin.

### Detect when the battery is charging

**Status:** proposed
**Scope:** `lib/Battery`, `src/mavlink.cpp`, `include/Data.h`, `src/logger.cpp`,
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
- Add the state to `Energy` in `include/Data.h` and dump it in `src/sdwrite.cpp`, so
  that charge cycles can later be reconstructed from the `.mpk` files. As with the
  temperature sensor, this changes the schema of the SD card files.
- If the software route is chosen, the history cannot grow: fixed-size buffer, no
  `malloc` per sample.
- Check in `test/test_main.cpp`, which runs on real hardware and can therefore
  validate the state with the board plugged in (charging) and unplugged.

### Download the SD files over MAVLink FTP

**Status:** proposed
**Scope:** `src/mavlink.cpp`, `lib/SdData`, `src/sdwrite.cpp`, `src/main.cpp`

Today the housekeeping log can only be recovered by pulling the card out of the
board: nothing exposes it over the link. Implementing MAVLink FTP
(`FILE_TRANSFER_PROTOCOL`) would allow listing and downloading `data0..N.mpk` and
`index.bin` from the ground with the reference GCS (`ftp list` / `ftp get` in
MAVProxy), which is the only realistic way of reading them with the satellite
assembled.

The slot is already marked: `src/mavlink.cpp` has an empty
`case MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL:` in the `TaskMavlink` switch.

Points to resolve before implementing:

- **Protocol scope.** MAVLink FTP is a state machine with sessions and opcodes
  (`ListDirectory`, `OpenFileRO`, `ReadFile`, `Terminate`, burst reads...). Decide
  the minimum subset: listing and reading read-only covers the use case; writing and
  deleting from the ground is another discussion, and a dangerous one over the file
  the firmware holds open.
- **Concurrency with `TaskSdWrite`.** `lib/SdData` keeps `_dataFile` open for
  writing while the logger dumps at 1 Hz, and the SD card hangs off SPI with `CS` on
  pin 9. Two tasks touching the card at once is corruption: either a mutex is
  needed, or FTP access goes through `TaskSdWrite`, which already owns the medium.
  This is the main design decision of this feature.
- **RAM.** The usual constraint: `mavlink_message_t` alone is ~290 bytes and the
  stacks are tight, between 96 and 256 words. An FTP task with its session buffer
  does not fit without measuring; the log high-water marks have to be checked before
  and after.
- **Download time.** By default `SdData` is 4 files of 1 GiB. Over a radio link,
  and with the ~239 useful bytes each FTP packet moves, downloading a whole one is
  not viable: that default size is worth revisiting, or supporting reads by offset
  to fetch only the stretch of interest. See *[The default SD ring is 4 GiB and
  never rotates]*.
- **Consistency of what is downloaded.** The active file is being written while it
  is read. Define whether it is served as is (the receiver may find a half-written
  MessagePack record at the end) or whether only the closed files of the ring are
  offered.
- Keep the identity triple of the rest of the firmware: system `1`,
  `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`.

### Debug and release builds, with MAVLink tracing on the console

**Status:** proposed
**Scope:** `platformio.ini`, `src/mavlink.cpp`, `src/serial.cpp`, `CLAUDE.md`,
`README.md`

Debugging the protocol today is done blind: there is no way to see which MAVLink
messages come in and go out without a GCS on the other end interpreting them. The
idea is to have two build profiles, with the debug one dumping the protocol trace
over the console port, in readable text.

Depends on the `move-mavlink-link-to-serial1` change: while the binary frames keep
going over USB there is no console port to write the trace to.

What it should trace, per message: direction (inbound/outbound), `msgid` — by name
if possible, not just the number —, source `sysid`/`compid` and length. The two
mandatory choke points already exist and are the natural places to hook it: the
`TaskMavlink` switch for what comes in and the `TaskSerialWrite` drain for what goes
out.

Points to resolve before implementing:

- **How the profiles are separated.** A second `[env:...]` in `platformio.ini`
  inheriting from the current one and adding its `build_flags` is the idiomatic
  PlatformIO way. Careful: CI runs `pio run` with no `-e`, which builds *every*
  environment — so the debug profile is checked on every push too, which is
  desirable, but it must not become the one flashed to the board by default.
- **The trace cannot exist in release.** It has to be compiled out with `#ifdef`,
  not left behind a runtime `if`: the text strings and the formatting take flash and
  RAM, and neither is spare here. The release profile must produce exactly today's
  binary.
- **Writing to the console cannot block the flight.** If USB is not connected or its
  buffer fills up, a `print` can block and drag down a `PRIORITY_HIGHEST` task. And
  several tasks writing at once interleave the output. Decide whether it is written
  directly, with a mutex, or through a queue like the rest of the firmware.
- **Stacks.** Formatting text consumes stack, and they are tight, between 96 and 256
  words. When enabling the debug profile the log high-water marks have to be checked
  again: this is exactly the case that triggers the slow blink of `src/hooks.cpp`.
- **What else goes into the debug profile.** A configurable detail level (headers
  only, or a hex dump), and whether it is reused for other subsystems' traces or
  stays MAVLink-only.
- Document in `README.md` and `CLAUDE.md` how to build and upload each profile.

### Add a watchdog

**Status:** proposed
**Scope:** `src/main.cpp`, tasks in `src/*.cpp`, `platformio.ini`

There is no watchdog. Any task that hangs — an `xQueueReceive` that never arrives,
an I2C transfer waiting for the DS1307, the loop in `src/hooks.cpp` — leaves the
satellite inert until a power cycle that nobody can perform in flight. For firmware
meant to fly it is the most serious omission in the project.

The RA4M1 has an independent WDT. To decide:

- **Which tasks feed it.** A single `refresh()` from the lowest priority task
  detects starvation but not that one specific task has stopped. A scheme where each
  task marks its pass and a single one refreshes when all have passed detects much
  more, at the cost of shared state.
- **Timeout**, against the slowest cycle (`TaskSdWrite`, which can block on SPI
  while writing to the card).
- **What happens after a watchdog reset.** Leave a trace in the SD log or in a
  `STATUSTEXT` at boot; otherwise the resets are invisible from the ground.
- Interaction with `configASSERT`: today a hardware failure in `setup()` hangs the
  board. With a watchdog that becomes an infinite reset loop, which may be better
  (it retries) or worse (it never gets to emit anything). It has to be decided at
  the same time.


### Report the satellite's real state in the heartbeat

**Status:** proposed
**Scope:** `src/mavlink.cpp`, `include/Data.h`

`sendHeartbeat()` sends constants: `MAV_STATE_ACTIVE` and
`MAV_MODE_FLAG_AUTO_ENABLED | MAV_MODE_FLAG_SAFETY_ARMED`, no matter what. With the
battery at 5 %, the SD card unmounted or the RTC lost, the satellite keeps
announcing over the link that everything is fine — exactly when the ground needs to
find out.

To decide:

- Which conditions raise the state to `MAV_STATE_CRITICAL` or
  `MAV_STATE_EMERGENCY`: battery threshold, SD write failure, invalid time.
- Where that information comes from. Nobody centralises system health today; shared
  state is needed, and a way to reach it without breaking the task and queue model.
- Whether `MAV_STATE_BOOT` during `setup()` and `MAV_STATE_STANDBY` without a link
  add anything, or whether active/critical is enough.

Related to *[Detect when the battery is charging]*: the charge state is one of the
natural inputs to this decision.


### Answer the GCS messages that are ignored today

**Status:** proposed
**Scope:** `src/mavlink.cpp`

The `TaskMavlink` switch has several `case` branches that only `break`:
`COMMAND_LONG` (including `MAV_CMD_GET_HOME_POSITION`), `PARAM_REQUEST_LIST` and
`REQUEST_DATA_STREAM`. The GCS gives them up for lost and retries: MAVProxy sits
waiting for a `COMMAND_ACK` that never arrives.

The bare minimum is to always answer something. A `COMMAND_ACK` with
`MAV_RESULT_UNSUPPORTED` is an honest answer and stops the retry; silence is not.

To decide:

- Which commands are really supported and which are explicitly rejected.
- **`AUTOPILOT_VERSION` (148)**, in response to
  `MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES` (520). It is what the GCS asks as soon as
  it connects, to know what the vehicle can do; with no answer it treats you as a
  minimal node. Cheap to implement and it improves everything else.
- **Telemetry rates from the ground.** Today they are hard-wired in `TaskHeartbeat`
  and `TaskMavlinkBatteryStatus`. Rather than implementing `REQUEST_DATA_STREAM`,
  which is deprecated, `MAV_CMD_SET_MESSAGE_INTERVAL` (511) with `MESSAGE_INTERVAL`
  (244) is the current mechanism. It matters over a narrow radio link, and even more
  so if *[Download the SD files over MAVLink FTP]* lands and competes for it.
- The parameter protocol has its own entry:
  *[Implement the MAVLink parameter protocol]*.


### Emit `SYS_STATUS`

**Status:** proposed
**Scope:** `src/mavlink.cpp`, `include/Data.h`

`SYS_STATUS` (1) is the most conspicuous absence in the current telemetry. It
carries the `onboard_control_sensors_present`, `_enabled` and `_health` bitmasks,
CPU load, battery voltage and percentage, and communication error counters. Every
GCS shows it front and centre; today the satellite sends none of it.

It fits with two things the firmware already has half done:

- The health bitmasks are the place to express "the SD card failed", "the RTC was
  lost", "the IMU does not answer". That is exactly what *[Report the satellite's
  real state in the heartbeat]* wants to communicate and the `HEARTBEAT` has no
  fields for. Both entries share the same source: a centralised health state, which
  does not exist today.
- `errors_count1..4` is where to keep the count of the failed `pvPortMalloc` calls
  of *[Check the result of `pvPortMalloc`...]* and of the sends dropped by a full
  queue, which are silently lost today.

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


### Publish housekeeping live with `NAMED_VALUE_INT` / `NAMED_VALUE_FLOAT`

**Status:** proposed
**Scope:** `src/mavlink.cpp`, `src/logger.cpp`

The whole housekeeping log exists to size the stacks, and today it can only be
consulted by pulling the card out of the board. `NAMED_VALUE_INT` (252) and
`NAMED_VALUE_FLOAT` (251) allow publishing the free heap and each task's high-water
mark over the link, live, without inventing custom messages or touching the `.mpk`
schema.

It is the cheap way to close the loop that `ARCHITECTURE.md` describes: the
high-water marks exist to size the stacks, so review them after changing a task
body. With this they are reviewed with the board assembled, rather than after the
fact.

To decide: which values are published and at what rate — there are eight fields and
the name takes 10 characters per message, so at 1 Hz this alone is not free over a
narrow link — and whether it should be exclusive to the profile of *[Debug and
release builds...]*.

Partial alternative: `MEMINFO` (152) for the heap, although it is ArduPilot-specific
and does not cover the per-task stacks.


## To change

### Check the result of `pvPortMalloc` in the four places that don't

**Status:** defined
**Scope:** `src/mavlink.cpp`

The project's queue protocol is only half honoured in `src/mavlink.cpp`: every
`xQueueSend` frees with `vPortFree` when it does not return `pdPASS`, but four
allocations do not check that `pvPortMalloc` returned anything before using the
pointer. They hand it straight to `mavlink_msg_*_pack`, which writes to it:

- `src/mavlink.cpp:20` — `sendHeartbeat()`
- `src/mavlink.cpp:41` — `SYSTEM_TIME`
- `src/mavlink.cpp:104` — `sendBatteryStatus()`
- `src/mavlink.cpp:201` — `TIMESYNC` reply

With the heap exhausted, `pvPortMalloc` returns `NULL` and the `pack` writes to
address 0. The correct pattern is already in the same file at `src/mavlink.cpp:61`
(`STATUSTEXT`), and in `src/logger.cpp:42` and `src/serial.cpp:50`: wrap everything
from the allocation to the `xQueueSend` in `if (msg != NULL) { ... }`.

It matters more than it looks because the first three are **periodic** senders: a
momentarily full heap does not give a one-off failure, it repeats it every cycle.

Made more likely by *[The two serial queues cannot fit in the FreeRTOS heap]*: with
8 KB of heap this is not a remote condition.

To decide: whether failing to allocate should leave a trace (a counter in `Data`
towards the SD log) or whether silently skipping the send is enough.

### The two serial queues cannot fit in the FreeRTOS heap

**Status:** defined
**Scope:** `src/main.cpp`, `src/serial.cpp`, `src/mavlink.cpp`, `platformio.ini`

`configTOTAL_HEAP_SIZE` on this port is `0x2000` — 8 KB — and task stacks, TCBs and
the queue structures themselves all come out of that same heap. With this
configuration a TCB is 96 bytes, and `heap_4` adds an 8-byte header to every block
and rounds up to 8, so **a task costs `4 × stack_words + 112` bytes**. The full
accounting at boot:

| | Bytes |
|---|---|
| The seven tasks (1152 words of stack in total) | 5392 |
| Idle task (128 words, `configMINIMAL_STACK_SIZE`) | 624 |
| Timer daemon (128 words) and its command queue | 864 |
| The three queue structures (`Queue_t` is 68 bytes, plus 16 × 4) | 432 |
| **Committed** | **7312** |
| **Free** | **~870** |

Every message travelling through `serialReadQueue` and `serialWriteQueue` is a
`mavlink_message_t`, which is packed and measures 291 bytes — 304 as a heap block.
So the real ceiling is **two or three messages in flight**, against the depth 16
that `src/main.cpp:61` and `src/main.cpp:64` declare for each of the two queues. The
32 slots are unreachable by a wide margin: `pvPortMalloc` fails long before a queue
reports itself full.

Two findings from the accounting that were not obvious before:

- Nothing in `src/`, `lib/` or the dependencies calls `xTimerCreate`. The timer
  daemon and its queue are 864 bytes paid for a feature the firmware does not use,
  and `-D configUSE_TIMERS=0` reclaims them outright.
- There is more RAM headroom than the 32 KB figure suggests. `arm-none-eabi-nm`
  puts `ucHeap` (8 KB) inside `.bss`, `g_heap` — the separate newlib malloc heap —
  at `0x20003ce0` (another 8 KB), and `g_main_stack` (1 KB) at `0x20007b00`, which
  leaves **7712 bytes between them that no section claims**. The RAM percentage
  PlatformIO prints after a build counts none of those three.

These numbers are computed from the map file and the kernel headers, not measured on
the board. The free-heap field `TaskLogger` already writes to the SD log is the
check.

The consequence is not a full queue with a clean `vPortFree`, which the code does
handle, but a `NULL` allocation — see *[Check the result of `pvPortMalloc`...]* —
and, in the meantime, a heap that can be exhausted by a burst of traffic from the
GCS.

To decide:

- Whether the queue depths drop to something the heap can really back, or whether
  `configTOTAL_HEAP_SIZE` is raised in `build_flags` (the RA4M1 has 32 KB of RAM
  total, so there is some room, but it is shared with everything the Arduino core
  and the SD and MAVLink libraries use).
- Whether it is worth queueing something smaller than the whole
  `mavlink_message_t`: `mavlink_msg_to_send_buffer` already runs in
  `TaskSerialWrite`, and the wire frame of a typical message is far smaller than the
  291-byte struct. Two routes, and they differ: the serialised frame is still a heap
  pointer, while *[Queue the message intent by value instead of a packed
  `mavlink_message_t`]* removes the allocation entirely. That entry owns the
  question now.
- Whether the free heap and the failed allocations reach the ground, which is what
  *[Emit `SYS_STATUS`]* proposes with `errors_count1..4`.

### Queue the message intent by value instead of a packed `mavlink_message_t`

**Status:** proposed
**Scope:** `src/mavlink.cpp`, `src/serial.cpp`, `src/main.cpp`, `include/Data.h`,
`CLAUDE.md`

Both serial queues carry a pointer to a `mavlink_message_t`, which is packed and
measures 291 bytes whatever the message is: it reserves 255 bytes of payload and 13
of signature every time, and the firmware signs nothing. A `HEARTBEAT` carries 9
useful bytes, `SYSTEM_TIME` 12, `BATTERY_STATUS` 41. The amplification runs between
7x and 32x, and it is paid out of the heap on every send.

The alternative is to queue what the producer *means* rather than what the wire
needs, and let the single consumer do the packing it already does:

```c
struct LinkMsg {
    uint8_t kind;
    union {
        struct { uint64_t unix_usec; uint32_t boot_ms; }  system_time;
        struct { uint16_t millivolts; int8_t remaining; } battery;
        struct { int64_t ts1; uint8_t tsys, tcomp; }      timesync;
        struct { uint8_t severity; char text[50]; }       statustext;
    };
};
```

That is 64 bytes, and `STATUSTEXT` is what sets the size — its text field is 50
characters. Without it the item would fit in 16 bytes. `HEARTBEAT` carries no
payload at all: the `kind` is the whole message.

At 64 bytes the item goes through the queue **by value**, and the consequence is the
point of the entry: `pvPortMalloc`, the `NULL` check, the `vPortFree` on a failed
`xQueueSend` and the `vPortFree` after use all stop existing. Five places where the
allocation protocol can be got wrong become zero, and the leak that `CLAUDE.md`
describes as fatal within minutes stops being an available mistake rather than a
watched one. The producer fills a local struct and sends it; the consumer receives a
local copy and packs it into a `static mavlink_message_t` in `.bss` — the pattern
`src/serial.cpp:29` already uses for the read path.

The ledger, against the accounting in *[The two serial queues cannot fit in the
FreeRTOS heap]*:

| | Today | Proposed |
|---|---|---|
| `serialWriteQueue` storage | 16 x 4 = 64 B | 8 x 64 = 512 B |
| In flight | 291 B per message, from ~870 B free | 0 |
| `.bss` | 0 | 291 B for the packing buffer |
| Heap operations per message | 3 (`malloc`, `free` on failure, `free` after use) | 0 |

It is not a net saving of bytes under normal load: about 448 bytes of fixed heap buy
back a transient demand of the same order. What it buys is that the demand becomes
**deterministic and reserved at boot** instead of competing, every second, for the
~870 bytes that are left — which is precisely the competition that makes
the `add-console-cli` change a repeating
failure rather than a one-off.

It also retires an invariant with its reason rather than by decree. `CLAUDE.md` says
queues carry heap pointers and never values, and that is correct **because the item
is 291 bytes**: by value it would need 4656 bytes of permanent storage. At 64 bytes
the arithmetic inverts. If this is implemented, `CLAUDE.md` has to say so, or the
next reader will follow a rule whose justification no longer holds.

To decide:

- **The depth.** 16 slots were never reachable. Producers emit about 2.5 messages a
  second and the drain handles roughly 100, so 4 slots (256 B) or 8 (512 B) both have
  wide margin; 4 leaves ~678 B free at boot against ~422 B for 8.
- **Whether `STATUSTEXT` stays in the union.** It quadruples the item on its own. A
  separate, shallower queue for it, or a bounded text pool, would take the common
  item down to 16 bytes — at the cost of a second queue structure.
- **Whether `serialReadQueue` changes too.** It has the same 291-byte item, but the
  producer is a parser that already owns a `mavlink_message_t`, so the argument is
  weaker there and the two can be decided separately.
- Whether the packing buffer is a `.bss` static or lives on `TaskSerialWrite`'s
  stack, which is 192 words and would have to grow by about 73.

Related: the USB endpoint proposed in the `add-usb-dual-protocol` change takes a
third route — no queue at all, packing into a stack buffer and dropping the frame
when the port has no room, following madflight. When that lands the firmware will
hold two outbound models at once, which is defensible (`UART` does not implement
`availableForWrite()` and `SerialUSB` does) but is worth converging deliberately
rather than by accretion.

**What this entry unblocks.** Converging them means giving the USB endpoint its own
write queue that the same producers fill, so one `HEARTBEAT` becomes one entry in each
of two queues and each port drains its own at its own pace. That is impossible while
the item is a `mavlink_message_t`: the block is 304 bytes in `heap_4`, there are about
870 bytes free after boot, and a producer would have to allocate twice — 608 bytes for
a single duplicated `HEARTBEAT`, and over 1200 if the battery sender fires in the same
instant, which does not fit. `pvPortMalloc` returns `NULL` long before either queue
reports itself full, and per *[Check the result of `pvPortMalloc` in the four places
that don't]* three of the four senders would write to address 0.

Two arguments say the queue is not obviously the better model even once it fits, and
both should be weighed rather than assumed:

- **A queue here converts a visible drop into an invisible allocation failure.** The
  appeal of queueing is not losing a message when the port is momentarily full, but
  with this much free heap the queue absorbs two messages and then fails. Dropping a
  frame is behaviour the USB endpoint's spec states outright; a failed allocation is
  something nothing currently detects.
- **It recouples the two links through the heap.** Today the USB path cannot consume
  heap at all. With a shared-heap queue, a USB queue nobody drains — because nobody
  plugged a cable in — starts costing the radio link its allocations. On a satellite,
  the radio's reliability should not depend on what is attached to the bench port.

Both arguments weaken sharply at 64 bytes an item: a depth-4 USB queue is 256 bytes
reserved once, and duplicating a message costs nothing at send time, so a queue that
nobody drains fails its `xQueueSend` and is ignored rather than exhausting anything.
That is the state in which converging the two models is worth doing, and it is why
this entry is the prerequisite rather than the two being independent.

Sequencing, if both are implemented: this entry moves the builder call from the
producer to the drain — the producer fills an intent, the consumer packs it — so it
revisits the split that `add-usb-dual-protocol` introduces in `src/mavlink.cpp`. The
shared builders themselves survive unchanged; only their call sites move. On the USB
side the convergence replaces that change's telemetry scheduler with a queue drain and
leaves everything else — mode detection, the `availableForWrite()` measurement, the
deleted `bench`, the HIL cases — untouched.

### The default SD ring is 4 GiB and never rotates

**Status:** defined
**Scope:** `lib/SdData`, `src/sdwrite.cpp`, `test/test_main.cpp`

`SdData`'s default constructor is `SdData(int files = 4, size_t size = 1024UL *
1024UL * 1024UL)`: four files of 1 GiB each, 4 GiB of card. The design intent is a
fixed-footprint ring so the card can never fill, but with these defaults the
opposite holds:

- The ring needs a card with 4 GiB free. On a smaller one, the first file simply
  grows until `SD.open` or the write fails, and that failure is silent — see *[SD
  logging failure is silent]*.
- One housekeeping record is on the order of a hundred bytes and `src/logger.cpp`
  writes one per second. Filling 1 GiB at that rate takes around 115 days, so
  rotation never actually happens in any realistic mission: `writeLogIndex()`,
  `index.bin` and the whole resume-after-power-cycle mechanism are effectively dead
  code that has never run in flight.
- It also makes *[Download the SD files over MAVLink FTP]* impractical: nothing that
  size comes down a telemetry radio.

Related: `test/test_main.cpp` constructs `SdData(TEST_FILE_COUNT,
TEST_FILE_SIZE_MB)` with `TEST_FILE_SIZE_MB = 1024UL`, which is bytes, not
megabytes — so the only place rotation is ever exercised is the test, by accident of
a misleading constant name. That constant is also listed in *[Minor leftovers
cleanup]*.

To decide: a default file size that actually rotates within a mission (a few
hundred kilobytes to a few megabytes puts rotation at hours or days), and whether
the size is a compile-time constant or a ground-settable parameter under *[Implement
the MAVLink parameter protocol]*.

### `SdData::begin()` is not idempotent, and the Unity tests delete its open file

**Status:** defined
**Scope:** `lib/SdData`, `test/test_libs/test_main.cpp`

`SdData::begin()` opens the log file only when it does not already hold one:

```cpp
_fileIdx = readLogIndex();
if (!_dataFile) {
    _dataFile = SD.open(logFileName.c_str(), FILE_WRITE);
}
```

There is no `end()` and nothing ever closes `_dataFile`, so a second `begin()` is a
no-op that silently keeps the first file — even when `_fileIdx` has changed and a
different file is what should be open.

`test/test_libs/test_main.cpp` walks straight into it. `setUp()` calls
`cleanSdFiles()` and then `begin()`; `tearDown()` calls `cleanSdFiles()` again, which
`SD.remove()`s a file `sdData` still has open. From the second case onwards `begin()`
sees a truthy `_dataFile` and does not reopen, so the remaining cases write through a
handle to a deleted file. The suite passes — it asserts on `SD.exists()`, not on the
bytes — which is what makes this worth writing down rather than noticing the day it
matters.

In flight `begin()` is called exactly once, from `TaskSdWrite`, so nothing is broken
today. It becomes real the moment anything restarts the logger: a card remount, an
error-recovery path, or the SD failure handling of *[SD logging failure is silent]*.

To decide: whether `begin()` closes and reopens unconditionally, or gains an `end()`
and the tests call it; and whether `cleanSdFiles()` should refuse to remove a file the
object still holds, which would have made this visible immediately.

Found by Copilot reviewing the pull request that split `test/` into `test_libs` and
`test_hil`. It is not a regression of that change: the code is untouched and only
moved.


### `setup()` asserts on the RTC before the console exists

**Status:** defined
**Scope:** `src/main.cpp`

`src/main.cpp:52` runs `configASSERT(systemTime.begin())` and only then
`src/main.cpp:54` runs `Serial.begin(115200)`. If the DS1307 does not answer, the
board hangs before the port through which it could report it has even been opened.
The same reasoning applies to `configASSERT(SD.begin(9))` right after: it fails
before any task exists that could emit a `STATUSTEXT`.

The result is a board that is dead and mute, and from the outside a missing RTC, a
missing card and a stack overflow all look identical: nothing on the link and no
LED.

Overlaps with *[The stack overflow hook hangs before it warns]* — the same problem
of a diagnostic that needs hardware which may be exactly what failed — and with
*[Add a watchdog]*, which changes what hanging in `setup()` means.

To decide: whether `Serial.begin()` simply moves to the first line of `setup()`
(cheap, and enough for a bench diagnosis), whether the failure is signalled on
`LED_BUILTIN` with a distinguishable pattern per cause, and whether the RTC or the
SD card really deserve halting the board rather than booting in a degraded state and
saying so over the link. This last one is a design decision, since `configASSERT`
halting rather than degrading is deliberate and documented in `ARCHITECTURE.md`.

### Improve clock synchronisation

**Status:** proposed
**Scope:** `lib/SystemTime`, `src/mavlink.cpp`

`lib/SystemTime` keeps the R4 internal RTC and the external DS1307 in time, but the
synchronisation has several limitations today that show up as soon as the satellite
has been powered for a while or the GCS tries to measure the offset.

Current limitations, in order of impact:

- **One second resolution.** `getUnixTimeUsec()` and `getUnixTimeNsec()` are
  `getUnixTime()` multiplied by 10^6 and 10^9, so the sub-second part is always
  zero. That goes into the `TIMESYNC` reply and into the `SYSTEM_TIME` emitted every
  second from `TaskHeartbeat`: the GCS receives a timestamp quantised to the second
  and its offset estimate inherits that error of up to ±1 s. Combining the RTC
  (seconds) with `xTaskGetTickCount()` or `micros()` for the fraction is what gives
  real resolution.
- **`TIMESYNC` time base not pinned down.** The reply at `src/mavlink.cpp:201` uses
  `getUnixTimeNsec()`, wall clock time. Whether that is what the reference GCS
  expects, or a monotonic time since boot, should be decided and documented, because
  if they do not match the offset computed on the ground means nothing.
- **The DS1307 is not corrected if the internal RTC is already in time.**
  `setUnixTime()` takes the early `return` after comparing only against the internal
  clock, so a DS1307 that has drifted is never readjusted from the ground — and it
  is precisely the one that seeds the time on the next boot.
- **There is no periodic resynchronisation.** `begin()` copies DS1307 → internal RTC
  once at boot and that is that. The two clocks drift apart for the whole mission
  with nobody bringing them back together.
- **What arrives from the GCS is not validated.** `MAVLINK_MSG_ID_SYSTEM_TIME` is
  accepted as is: a corrupt or zero value leaves the satellite in 1970 and
  contaminates the `unixtime` of every SD record, which is the reference the `.mpk`
  files are later read against.

To decide before implementing: which clock wins when they disagree, how often they
resynchronise with each other, and what date range is considered acceptable in an
incoming `SYSTEM_TIME`.

Touching `lib/SystemTime` means running `pio test`: `test/test_main.cpp` validates
against the real DS1307, so this cannot be checked without the board.

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

### The SD log never stores the battery data

**Status:** defined
**Scope:** `src/logger.cpp`

In `src/logger.cpp` the `Data` initialiser fills `unixtime`, `uptime` and `system`,
but **not `energy`**. The file does not even include `Battery.h` or declare
`extern Battery battery`. Being aggregate initialisation, the missing members are
zeroed, so `src/sdwrite.cpp` writes `millivolts: 0` and `remaining: 0` on every
sample, always.

In other words: the energy telemetry **is in none of the `.mpk` files recorded so
far**, even though the field appears in them. The voltage does go out over MAVLink
in `BATTERY_STATUS`, so the fault goes unnoticed with the GCS in front of you; it
only shows when opening the files.

The fix is to declare the `extern`, include the header and fill `energy` with
`battery.millivolts()` and `battery.remaining()`. `lib/Battery` already caches for
125 ms, so calling it at 1 Hz from `TaskLogger` adds no ADC reads.

Careful with one thing: that cache (`_cachedVoltage`, `_lastRead`) is not guarded,
and today only `TaskMavlinkBatteryStatus` touches it. Reading it from `TaskLogger`
as well makes `Battery` shared state between two tasks of different priorities. The
worst case is benign — a torn read of a `float` and a stale timestamp, not
corruption of anything else — but it should be a deliberate decision, not an
accident.

When touching it, check `TaskLogger`'s high-water mark: it is 96 words, among the
tightest in the project.


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

Overlaps with *[Add a watchdog]*: with a watchdog, sitting here blinking forever
stops being the obvious answer to an overflow. Also with *[`setup()` asserts on the
RTC before the console exists]*, which is the same class of problem.


### `uptime` overflows after ~49.7 days

**Status:** proposed
**Scope:** `src/logger.cpp`, `include/Data.h`

`uptime` is computed as `xTaskGetTickCount() * portTICK_PERIOD_MS` into a
`uint32_t`. With `configTICK_RATE_HZ` at 1000 and 32-bit ticks, the counter wraps
after ~49.7 days. On a mission lasting months the field stops meaning anything right
when it starts to be interesting.

The firmware's `vTaskDelayUntil` calls tolerate the wrap by design; the log field
does not.

To decide: keep a wrap count and store the uptime in 64 bits, or record a boot
counter plus the time since the last boot instead, which would also serve to detect
the resets of *[Add a watchdog]*.


### SD logging failure is silent

**Status:** proposed
**Scope:** `lib/SdData`, `src/sdwrite.cpp`, `src/mavlink.cpp`

If `SD.open` fails in `SdData::begin()`, the object is left with no file and
`write()` returns without doing anything, indefinitely. Neither `begin()` nor
`write()` return anything, and `TaskSdWrite` cannot tell "stored" from "thrown
away".

Result: the entire mission log can be lost without a single warning over the link.
`setup()` does use `configASSERT(SD.begin(9))`, but that only covers boot; a card
that fails or is unmounted later goes unnoticed.

To decide: having `begin()`/`write()` return a result and `TaskSdWrite` propagate
it; and how the ground finds out — a `STATUSTEXT`, a field in the heartbeat of
*[Report the satellite's real state in the heartbeat]*, or both. Be careful not to
flood the link by repeating the warning at 1 Hz.


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


### Review the contents of the messages already emitted

**Status:** defined
**Scope:** `src/mavlink.cpp`

The four messages the satellite emits today go out with empty, constant or outright
misleading fields. No new hardware is needed to fix a good part of it:

- **`STATUSTEXT` is misused, not just badly formatted.** Beyond the bug in *[Fix the
  pointer arithmetic...]*, the design problem is that the `default` branch of the
  switch answers the ground with a text message **for every inbound message not
  covered**. A talkative GCS continuously sends things the switch does not cover
  (`MISSION_REQUEST_LIST`, `PARAM_REQUEST_READ`, `MISSION_COUNT`...), so the
  satellite spends its time flooding a narrow link with complaints. Take it out of
  there and reserve `STATUSTEXT` for what deserves a warning: the result of
  initialisation at boot, an SD failure, the cause of the last reset.
- **`BATTERY_STATUS` goes out nearly empty:** `current_battery`, `current_consumed`
  and `energy_consumed` at `-1`, `time_remaining` at `0`, `temperature` at
  `INT16_MAX` and `charge_state` at `MAV_BATTERY_CHARGE_STATE_UNDEFINED`. Two of
  them can be filled with no additional hardware as soon as *[Add the GY-87 IMU]*
  (temperature) and *[Detect when the battery is charging]* (charge state) land.
  `time_remaining` requires measuring current.
- **`HEARTBEAT` declares things that are not so:** `MAV_MODE_FLAG_SAFETY_ARMED |
  MAV_MODE_FLAG_AUTO_ENABLED` fixed and `custom_mode` at 0, no matter what. The
  state is covered by *[Report the satellite's real state in the heartbeat]*; the
  mode flags are a separate decision.
- **`MAV_TYPE_ROCKET` is debatable.** There is no `MAV_TYPE_SATELLITE`, but
  `MAV_TYPE_GENERIC` describes a CubeSat better than a rocket does, and it changes
  how the GCS draws it. Worth deciding soon: `ARCHITECTURE.md` fixes it as the bus
  identity and the more code assumes it, the more it costs to change.
- **`SYSTEM_TIME` at 1 Hz is a lot** for something that hardly ever changes in an
  interesting way. If `MAV_CMD_SET_MESSAGE_INTERVAL` lands in *[Answer the GCS
  messages that are ignored today]*, this solves itself.


### Minor leftovers cleanup

**Status:** defined
**Scope:** `src/mavlink.cpp`, `src/logger.cpp`, `src/hooks.cpp`,
`test/test_main.cpp`

Small, unrelated things worth getting out of the way in one go:

- `src/mavlink.cpp` declares `extern RTC_DS1307 rtc;`, a global that exists nowhere.
  It does not fail to link only because nobody uses it.
- `src/logger.cpp` initialises `Data` with the GNU label syntax (`unixtime: ...`),
  an extension that recent GCC versions reject in C++. The C++20 designated
  initialisers (`.unixtime = ...`) are the standard equivalent.
- `src/mavlink.cpp:175` declares `mavlink_command_long_t command;` inside a `case`
  with no braces of its own, which puts a declaration in the scope of the rest of
  the switch. It compiles because it has no initialiser. Note that cppcheck does
  **not** flag it: the `add-static-analysis-to-ci` change measured what the checker
  actually reports, and this is not in it. It does report the GCC initialiser syntax
  above, as three `unusedLabel` findings in `src/logger.cpp`.
- `test/test_main.cpp` uses `StaticJsonDocument`, deprecated in ArduinoJson 7, while
  `src/sdwrite.cpp` already uses `JsonDocument`.
- The test constant `TEST_FILE_SIZE_MB` is `1024UL`, which is bytes, not megabytes:
  the name misleads about what is really being tested. See *[The default SD ring is
  4 GiB and never rotates]*.
- A space is missing in `"Overflow on" + String(pcTaskName)` in `src/hooks.cpp`.

### The tests do not link FreeRTOS, so nothing covers the tasks

**Status:** defined
**Scope:** `platformio.ini`, `test/test_main.cpp`, `CLAUDE.md`, `ARCHITECTURE.md`

`test_build_src` defaults to `False` in PlatformIO, and `platformio.ini` does not
set it. `src/` is therefore not compiled into the test binary: no `main.cpp`, no
`xTaskCreate`, no `vTaskStartScheduler`. The five cases link `Battery`,
`SystemTime` and `SdData` against the Arduino core and nothing else.

The section sizes confirm it. The application firmware has a `.bss` of 14824 bytes,
which contains `ucHeap`; the test firmware built from the same tree has a `.bss` of
4608 bytes, too small to hold an 8192-byte array. The FreeRTOS heap is not in the
test binary because FreeRTOS is not in the test binary.

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
