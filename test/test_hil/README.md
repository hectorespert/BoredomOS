# Hardware-in-the-loop checks

These run **on the development machine** and interrogate the satellite over the
MAVLink link. Nothing here is compiled or flashed: the firmware under test is
whatever `pio run -t upload` last put on the board, running exactly as it would in
flight.

That is the point. `test_libs` links `lib/` against the Arduino core and cannot
observe the RTOS at all — no task, no queue, no stack. These checks see only what
the ground station sees, which is the firmware's actual contract. They exercise the
`mavlink-link` capability, but which scenario each case covers is not recorded: nine
cases against eight scenarios, and six of the cases name none. `TODO.md` carries the
entry for fixing that.

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

## Under `pio test`

`pio test` runs this suite: it builds `src/`, flashes it, and then runs `run.py`
against the board. `pio test -e bench` does the same with the link moved to USB, so
no adapter is needed. The Unity suite is opt-in as `pio test -e libs`, because that
one replaces the firmware and erases the card.

Two details make it work, and neither is obvious:

- **`build_stub.cpp`.** PlatformIO counts the sources it compiled from the suite
  directory and bails out before it ever reaches `src/`, so `test_build_src = yes` is
  not enough on its own — a suite that is entirely host-side Python still needs one
  translation unit to exist.
- **`find_port()` waits.** Under `pio test` the checks start seconds after the
  upload, and a USB CDC port takes a moment to re-enumerate after the board resets.
  Without the wait every case is skipped for no real reason.

## One thing not to do

Do not use a 1200-baud touch to reset this board. On the UNO R4 Minima that enters
DFU and stays there — it does not time out into the sketch the way AVR boards do —
and the only way back is to flash.
