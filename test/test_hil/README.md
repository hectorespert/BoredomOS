# Hardware-in-the-loop checks

These run **on the development machine** and interrogate the satellite over the
MAVLink link. Nothing here is compiled or flashed: the firmware under test is
whatever `pio run -t upload` last put on the board, running exactly as it would in
flight.

That is the point. `test_libs` links `lib/` against the Arduino core and cannot
observe the RTOS at all — no task, no queue, no stack. These checks see only what
the ground station sees, which is the firmware's actual contract, and they map
one-to-one onto the scenarios in `openspec/specs/mavlink-link/spec.md`.

## Running them

```bash
pip install -r requirements.txt

python run.py                       # every case
python run.py --list                # show the cases without connecting
python run.py --filter heartbeat    # only cases whose name matches
python run.py --port /dev/ttyUSB0   # explicit port (or set HIL_PORT)
```

The port is found automatically: `HIL_PORT` if set, then an Arduino CDC device,
then a lone USB serial adapter. Baud defaults to 57600, matching `LINK_BAUD` in
`include/Link.h`; a CDC port ignores it, so the same default works either way.

**The link has to be reachable from this machine.** With the default build it is on
`Serial1` (D0/D1), which means a USB-TTL adapter or the radio. To use USB instead,
flash the bench build:

```bash
PLATFORMIO_BUILD_FLAGS="-D LINK_SERIAL=Serial -D LINK_BAUD=115200" pio run -t upload
```

## Output

Unity's line format — `file:line:name:PASS|FAIL|IGNORE[: message]` — so `pio test`
would count and colour these natively if the suite were wired in as a
`test_testing_command`. Exit status is 1 if any case failed.

A run where nothing could be checked reports every case as `IGNORE` and exits **0**.
Absent hardware is not a red build; a real failure is.

Every case is gated behind a working link, including `test_usb_console_is_silent`,
which strictly speaking only needs the USB port. That is deliberate: with no link
reachable, "the USB console is silent" cannot tell a correctly moved link from a dead
board, and a green result there would be a false pass. The meaningful claim is frames
on the link *and* silence on USB, so the check only runs when the first half holds.

## Adding a case

Any `test_*` function in a `check_*.py` file, taking the shared `link`. Fail by
raising: `AssertionError` for something wrong, `hil.NoLinkError` for something that
cannot be checked here. `link.sample()` returns one shared 12-second observation, so
the rate checks cost one wait between them rather than one each.

## Not wired into `pio test`

Deliberately. Doing that needs an `[env:hil]` with `test_testing_command` pointing at
`run.py` and `test_build_src = yes`, and a decision about what a bare `pio test`
should mean — today it means the Unity suite, which reflashes the board and erases
the SD card. Inverting that so the destructive suite is the opt-in one is the open
question.

## One thing not to do

Do not use a 1200-baud touch to reset this board. On the UNO R4 Minima that enters
DFU and stays there — it does not time out into the sketch the way AVR boards do —
and the only way back is to flash.
