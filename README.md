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
| Ground link | USB CDC, 115200 baud |

Wiring is hardcoded in the firmware, not configurable.

## Quick start

Requires [PlatformIO](https://platformio.org/install/cli).

```bash
pio run                  # build
pio run -t upload        # flash the board
pio device monitor       # serial at 115200 — raw MAVLink frames, not text
```

Tests run **on the board only**: they assert against a real battery, RTC and SD
card, so they need the assembled hardware and cannot run in CI.

```bash
pio test
```

## Talking to it

MAVProxy is the reference ground control station:

```bash
mavproxy.py --master=/dev/ttyACM0,115200 --load-module system_time
```

The satellite identifies itself as system `1`, component `MAV_COMP_ID_AUTOPILOT1`,
type `MAV_TYPE_ROCKET`. It emits `HEARTBEAT` and `SYSTEM_TIME` at 1 Hz and
`BATTERY_STATUS` every 2 s, and its clock can be set from the ground with
`SYSTEM_TIME` or `TIMESYNC`.
