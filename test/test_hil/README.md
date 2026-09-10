# Hardware-in-the-loop checks

These scripts run **on the development machine** and interrogate the satellite over
the MAVLink link. Unlike `test_libs`, nothing here is compiled or flashed: the
firmware under test is whatever `pio run -t upload` last put on the board, running
exactly as it would in flight.

That is the point. `test_libs` links `lib/` against the Arduino core and cannot
observe the RTOS at all — no task, no queue, no stack. These scripts see only what
the ground station sees, which is the firmware's actual contract.

## Status: ad hoc, not yet a suite

They are the scripts that verified the `move-mavlink-link-to-serial1` change, kept
here rather than lost. They are **not** wired into `pio test`:

- Each is run by hand and prints to stdout; there is no runner, no pass/fail
  aggregation and no exit code to automate against.
- Three of the four hardcode `/dev/ttyACM0` at 115200. Only `check_telemetry.py`
  takes the port, baud and duration as arguments.
- They need `pymavlink`, which the project does not declare as a dependency.

Making them a real suite means a `run.py` that prints Unity-format lines
(`file:line:name:PASS`), a `[env:hil]` with `test_testing_command` pointing at it,
and `test_build_src = yes` so the real firmware is what gets flashed. That is
deliberately not done yet.

## Running them

The link must be reachable from this machine. With the default build the link is on
`Serial1` (D0/D1), so that means a USB-TTL adapter or the radio. To use USB instead,
flash the bench build:

```bash
PLATFORMIO_BUILD_FLAGS="-D LINK_SERIAL=Serial -D LINK_BAUD=115200" pio run -t upload
```

Then, with `pymavlink` available:

```bash
python check_telemetry.py /dev/ttyACM0 115200 15   # message rates and identity
python check_clock.py                              # inbound SYSTEM_TIME sets the clock
python check_timesync.py                           # TIMESYNC is answered
python check_silence.py                            # the port carries nothing
```

`check_silence.py` is the inverse check: pointed at USB with the **default**
(`Serial1`) build it must report zero bytes, which is what shows the link really left
USB.

## One thing not to do

Do not use a 1200-baud touch to reset this board. On the UNO R4 Minima that enters
DFU and stays there — it does not time out into the sketch the way AVR boards do —
and the only way back is to flash. The script that did it is deliberately not kept
here.
