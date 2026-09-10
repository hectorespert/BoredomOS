## Why

MAVLink and debugging share one port today. `Serial` (USB CDC) carries the binary
frames, so `pio device monitor` shows unreadable bytes and any debug `print` would
corrupt the link. The satellite also has no path to a telemetry radio: USB is not a
flight interface.

Moving the link to the `Serial1` hardware UART separates the two uses and puts the
link on the pins the radio will use. It also exposes a scheduling problem that USB
hid: `UART::write()` in the Renesas core busy-waits until the byte is on the wire,
and `TaskSerialWrite` runs at `PRIORITY_HIGHEST`, so every frame would block the
whole system for the duration of its transmission.

## What Changes

- The MAVLink link moves from `Serial` (USB CDC) to `Serial1`, the hardware UART on
  pins D0 (`RX`) and D1 (`TX`).
- The port and its baud rate are named once, in a new `include/Link.h`, as
  `LINK_SERIAL` and `LINK_BAUD`. Both are overridable from `build_flags`, so
  `-D LINK_SERIAL=Serial` returns the link to USB without touching protocol code.
- `TaskSerialWrite` drops from `PRIORITY_HIGHEST` to `PRIORITY_HIGH`.
- The two `while (!Serial)` guards in `src/serial.cpp` and `waitSerial()` with its
  three call sites in `src/mavlink.cpp` are removed. On a `UART` the core's
  `operator bool()` returns `true` unconditionally, so they are not a wait — they are
  dead code.
- `Serial` stays open at 115200 as a text console. Nothing writes to it while tasks
  are running; the only writer remains the stack-overflow hook in `src/hooks.cpp`,
  unchanged by this change.
- **BREAKING** for the ground station: `mavproxy.py --master=/dev/ttyACM0` no longer
  sees the vehicle. The link is reachable over the UART pins, or over USB again with
  `-D LINK_SERIAL=Serial`.

Deliberately **not** in this change: mirroring MAVLink onto USB, a CLI, protocol
autodetection on the USB input, and any policy for diagnostics over `STATUSTEXT` or
raw text.

## Capabilities

### New Capabilities

- `mavlink-link`: which port carries the MAVLink link, at what speed, how it is
  selected at build time, and what the firmware may assume about the port being
  ready.

### Modified Capabilities

None. `openspec/specs/` is empty; this change introduces the first capability.

## Impact

**RAM.** No task, queue or library is added or removed, so the cost against the 8 KB
FreeRTOS heap is **zero**. Task count stays at seven and total stack at 1152 words.

Static RAM does grow: instantiating the `UART` brings its two `SafeRingBufferN<512>`
buffers into `.bss`. Measured with `pio run`, total RAM goes from 15584 bytes on the
`-D LINK_SERIAL=Serial` build to 16828 on the default build — **+1244 bytes**, 51.4%
of the 32 KB. None of it comes out of the FreeRTOS heap.

**MAVLink surface.** Unchanged. Same message set, same rates (`HEARTBEAT` and
`SYSTEM_TIME` at 1 Hz, `BATTERY_STATUS` every 2 s), same identity triple: system id
`1`, `MAV_COMP_ID_AUTOPILOT1`, `MAV_TYPE_ROCKET`. Only the transport moves.

**Wiring.** Adds two pins: D0 (`RX`) and D1 (`TX`), the `UART1_RX_PIN` /
`UART1_TX_PIN` of the MINIMA variant. No conflict with SPI (11/12/13), the SD `CS`
(9), I²C (A4/A5) or the battery sense (A0). `ARCHITECTURE.md` must record them.

**Code.** `include/Link.h` (new), `src/serial.cpp`, `src/mavlink.cpp`,
`src/main.cpp`, `platformio.ini`, `README.md`, `ARCHITECTURE.md`.

**Verification.** No USB-TTL adapter is available, so the UART path cannot be
exercised on hardware in this change. What can be checked is `pio run` and a
functional run with `-D LINK_SERIAL=Serial`, which proves the refactor did not break
the protocol but does **not** prove the UART. That gap is stated in `tasks.md` and
must not be reported as verified.
