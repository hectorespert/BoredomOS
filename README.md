# BoredomOS

[![BoredomOS CI](https://github.com/hectorespert/BoredomOS/actions/workflows/main.yml/badge.svg)](https://github.com/hectorespert/BoredomOS/actions/workflows/main.yml)

Software and documentation for a CubeSat based on [https://www.thingiverse.com/thing:4096437](https://www.thingiverse.com/thing:4096437)

The satellite runs FreeRTOS on an Arduino UNO R4 Minima and presents itself to the
ground as a MAVLink vehicle: it emits periodic telemetry, accepts a small set of
commands, and keeps a housekeeping log on an SD card.

- **[ARCHITECTURE.md](ARCHITECTURE.md)** — how the firmware is built and why.
- **[TODO.md](TODO.md)** — planned features and pending changes.

## Hardware

| Part | Connection |
|---|---|
| Arduino UNO R4 Minima (Renesas RA4M1) | — |
| microSD card module | SPI, `CS` on pin **9** |
| DS1307 real-time clock | I2C, address `0x68` |
| Solar charger with LiPo cell | battery sense on **A0** |
| Ground link (MAVLink), primary | `Serial1` UART, **D0** (`RX`) / **D1** (`TX`), 57600 baud |
| Ground link (MAVLink), secondary | USB CDC — always on, no line rate |

Wiring is hardcoded in the firmware, not configurable. The one exception is the
link ports themselves: `include/Link.h` defines `LINK_UART`, `LINK_USB` and
`LINK_BAUD`, all overridable from `build_flags` (see `platformio.ini`).
`LINK_BAUD` applies to the UART alone — a CDC port has no line rate.

## Quick start

Requires [PlatformIO](https://platformio.org/install/cli).

```bash
pio run                  # build
pio run -t upload        # flash the board
```

Tests run **on the board only**: they assert against a real battery, RTC and SD
card, so they need the assembled hardware and cannot run in CI.

```bash
pio test
```

## Talking to it

MAVProxy is the reference ground control station:

The firmware answers MAVLink on **both ports at once**, with no build flag to
choose between them. Over the USB cable, with nothing else attached:

```bash
mavproxy.py --master=/dev/ttyACM0 --load-module system_time
```

Or over the UART, to whatever is wired to D0/D1 — the telemetry radio, or a
USB-TTL adapter on the bench:

```bash
mavproxy.py --master=<uart port>,57600 --load-module system_time
```

Both can be connected at the same time. Each stream numbers its own frames, and a
request is answered on the port it arrived on.

There is **no text console**. `pio device monitor` shows binary MAVLink frames.
The `ps` and `free` commands a previous version answered on USB are gone with
`src/cli.cpp`; free heap, minimum-ever-free heap and each task's stack high-water
mark are published as `NAMED_VALUE_INT` instead, once a ground station asks for
them with `MAV_CMD_SET_MESSAGE_INTERVAL` on message id 252.

The satellite identifies itself as system `1`, component `MAV_COMP_ID_AUTOPILOT1`,
type `MAV_TYPE_ROCKET` — one vehicle, on whichever port you attach to. It emits
`HEARTBEAT` and `SYSTEM_TIME` at 1 Hz and `BATTERY_STATUS` every 2 s, and its
clock can be set from the ground with `SYSTEM_TIME` or `TIMESYNC`.
